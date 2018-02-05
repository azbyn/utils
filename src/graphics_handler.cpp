#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <memory>

#include "scope_guard.h"
#include "graphics_handler.h"

namespace kmswm {
Result ModesetBuf::CreateFb(FD fd, uint32_t width, uint32_t height) {
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	Result ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = width;
	creq.height = height;
	creq.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
		return Error("cannot create dumb buffer");
	}
	this->stride = creq.pitch;
	this->size = creq.size;
	this->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	if (drmModeAddFB(fd, width, height, 24, 32, this->stride,
	                 this->handle, &this->fb)) {
		ret = Error("cannot create framebuffer");
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = this->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		ret = Error("cannot map dumb buffer");
		goto err_fb;
	}

	/* perform actual memory mapping */
	this->map = (Color *)mmap(0, this->size, PROT_READ | PROT_WRITE, MAP_SHARED,
	                          fd, mreq.offset);
	if (this->map == MAP_FAILED) {
		ret = Error("cannot mmap dumb buffer");
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(this->map, 0, this->size);

	return Result::Success();

err_fb:
	drmModeRmFB(fd, this->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = this->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}
void ModesetBuf::DestroyFb(FD fd) {
	struct drm_mode_destroy_dumb dreq;

	/* unmap buffer */
	munmap(this->map, this->size);

	/* delete framebuffer */
	drmModeRmFB(fd, this->fb);

	/* delete dumb buffer */
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = this->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}
ModesetDev::ModesetDev(uint32_t connId) : connId(connId) {

}
ModesetDev::~ModesetDev() {
}

Expected<ModesetDev> ModesetDev::create(const std::vector<ModesetDev>& modesets,
                                        FD fd, const drmModeRes *res, const drmModeConnector *conn) {
	ModesetDev dev(conn->connector_id);
	auto ret = dev.SetupDev(modesets, fd, res, conn);
	if (ret.HasError())
		return ret.GetError();
	return dev;
}

Result ModesetDev::SetupDev(const std::vector<ModesetDev>& modesets,
                            FD fd, const drmModeRes *res, const drmModeConnector *conn) {
	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		return Error(ENOENT, "ignoring unused connector %u",
		             conn->connector_id);
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		return Error(EFAULT, "no valid mode for connector %u",
		             conn->connector_id);
	}

	/* copy the mode information into our device structure */
	memcpy(&this->mode, &conn->modes[0], sizeof(this->mode));
	this->width  = conn->modes[0].hdisplay;
	this->height = conn->modes[0].vdisplay;
	//new(&bufs[0]) ModesetBuf(conn->modes[0].hdisplay, conn->modes[0].vdisplay);
	//new(&bufs[1]) ModesetBuf(conn->modes[0].hdisplay, conn->modes[0].vdisplay);

	printf("mode for connector %u is %ux%u\n",
	       conn->connector_id, this->width, this->height);

	/* find a crtc for this connector */
	auto ret = FindCrtc(modesets, fd, res, conn);
	if (ret.HasError()) {
		return Error(ret.GetError(), "no valid crtc for connector %u",
		             conn->connector_id);
	}

	/* create a framebuffer for this CRTC */
	ret = this->bufs[0].CreateFb(fd, width, height);
	if (ret.HasError()) {
		return Error(ret.GetError(), "cannot create framebuffer for connector %u",
		             conn->connector_id);
	}
	ret = this->bufs[1].CreateFb(fd, width, height);
	if (ret.HasError()) {
		this->bufs[0].DestroyFb(fd);
		return Error(ret.GetError(), "cannot create framebuffer for connector %u",
		             conn->connector_id);
	}


	return Result::Success();
}
Result ModesetDev::FindCrtc(const std::vector<ModesetDev>& modesets,
                            FD fd, const drmModeRes *res, const drmModeConnector *conn) {
	drmModeEncoder *enc;
	int32_t crtc;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			crtc = enc->crtc_id;
			for (auto& iter : modesets) {
				if (iter.crtcId == crtc) {
					crtc = -1;
					break;
				}
			}

			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				this->crtcId = crtc;
				return Result::Success();
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (int i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			Error("cannot retrieve encoder %u:%u",
			      i, conn->encoders[i]).Print();
			continue;
		}

		/* iterate all global CRTCs */
		for (int j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (auto& iter : modesets) {
				if (iter.crtcId == crtc) {
					crtc = -1;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (crtc >= 0) {
				drmModeFreeEncoder(enc);
				this->crtcId = crtc;
				return Result::Success();
			}
		}

		drmModeFreeEncoder(enc);
	}
	return Error(ENOENT, "cannot find suitable CRTC for connector %u",
	             conn->connector_id);
}

GraphicsHandler::GraphicsHandler(const GraphicsHandler& rhs) :
	fd(rhs.fd),
	modesets(rhs.modesets) {}

GraphicsHandler::GraphicsHandler(GraphicsHandler&& rhs) :
	fd(rhs.fd),
	modesets(std::move(rhs.modesets)) {}

void GraphicsHandler::ThreadJoin() {
	thread.join();
}
void GraphicsHandler::ThreadInit() {
	running = true;
	thread = std::thread(&GraphicsHandler::Update, this);
}
void GraphicsHandler::Stop() {
	running = false;
}
GraphicsHandler::~GraphicsHandler() {
	printf("GraphicsHandler dtor\n");
	Stop();
	Cleanup();
	close(fd);
}

static uint8_t next_color(bool *up, uint8_t cur, unsigned int mod) {
	uint8_t next = cur + (*up ? 1 : -1) * (rand() % mod);
	if ((*up && next < cur) || (!*up && next > cur)) {
		*up = !*up;
		next = cur;
	}

	return next;
}
uint8_t smiley_face[8][8] = {
	{0,0,1,1,1,1,0,0},
	{0,1,0,0,0,0,1,0},
	{1,0,1,0,0,1,0,1},
	{1,0,0,0,0,0,0,1},
	{1,0,1,0,0,1,0,1},
	{1,0,0,1,1,0,0,1},
	{0,1,0,0,0,0,1,0},
	{0,0,1,1,1,1,0,0},
};
Color smileyFacePixels[8][8] = {};

void GraphicsHandler::Update() {
	printf("Update\n");
	for (int i = 0; i < 8; ++i) {
		for (int j = 0; j < 8; ++j) {
			smileyFacePixels[i][j] = smiley_face[i][j] ? Colors::yellow : Colors::black;
		}
	}

	uint8_t r, g, b;
	bool r_up, g_up, b_up;

	srand(time(NULL));
	r = rand() % 0xff;
	g = rand() % 0xff;
	b = rand() % 0xff;
	r_up = g_up = b_up = true;

	for (int i = 0; i < 20; ++i) {
		//while (running) {
		r = next_color(&r_up, r, 20);//4);
		g = next_color(&g_up, g, 10);//2);
		b = next_color(&b_up, b, 5);///1);
		for (auto& iter : modesets) {
			auto& buf = iter.bufs[iter.frontBuf ^ 1];
			for (uint32_t j = 0; j < iter.height; ++j) {
				for (uint32_t k = 0; k < iter.width; ++k) {
					uint32_t off = (buf.stride / 4) * j + k;
					if (j < 64 && k < 64)
						buf.map[off] = smileyFacePixels[j%8][k%8];
					else
						buf.map[off] = Color(r, g, b);
				}
			}
			auto ret = drmModeSetCrtc(fd, iter.crtcId, buf.fb, 0, 0,
			                          &iter.connId, 1, &iter.mode);
			if (ret)
				Error("cannot flip CRTC for connector %u",
				      iter.connId).Throw();
			else
				iter.frontBuf ^= 1;
		}
		usleep(100000);
	}
	/*
	for (auto& iter : modesets) {
		for (uint32_t j = 0; j < iter.height; ++j) {
			for (uint32_t k = 0; k < iter.width; ++k) {
				uint32_t off = iter.stride * j + k * 4;
				*(uint32_t*)&iter.map[off] = 0xff5000;// (0xff << 16) | (0x << 8) | b;
			}
		}
	}
	*/
	//Cleanup();
}
Expected<FD> GraphicsHandler::Open(const char *node) {
	uint64_t has_dumb;

	FD fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return Error("cannot open '%s'", node);
	}

	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	        !has_dumb) {
		auto result = Error(EOPNOTSUPP, "drm device '%s' does not support dumb buffers",
		                    node);
		close(fd);
		return result;
	}
	return fd;
}

Expected<GraphicsHandler *> GraphicsHandler::create(const char *device) {
	GraphicsHandler *gh = new GraphicsHandler;
	auto a = gh->Main(device);
	if (a.HasError())
		return a.GetError();
	return gh;
}

Result GraphicsHandler::Main(const char *card) {
	printf("using card '%s'\n", card);

	/* open the DRM device */
	auto _fd = Open(card);
	if (_fd.HasError()) {
		return _fd.GetError();
	}

	this->fd = _fd.Get();
	/* prepare all connectors and CRTCs */
	Result result = Result::Success();
	auto ret = Prepare();
	if (ret.HasError())
		result = ret.GetError();

	/* perform actual modesetting on each found connector+CRTC */
	for (auto& iter : modesets) {
		iter.savedCrtc = drmModeGetCrtc(fd, iter.crtcId);
		printf("front =%d\n", iter.frontBuf);
		auto& buf = iter.bufs[iter.frontBuf];
		if (drmModeSetCrtc(fd, iter.crtcId, buf.fb, 0, 0,
		                   &iter.connId, 1, &iter.mode)) {
			result = Error("cannot set CRTC for connector %u", iter.connId);
		}
	}

	//Update();

	if (result.HasError()) {
		return result;// Error(ret, "modeset failed with error");
	}
	printf("end of Main\n");
	return result;
}

Result GraphicsHandler::Prepare() {
	drmModeRes *res;
	drmModeConnector *conn;

	/* retrieve resources */
	res = drmModeGetResources(fd);
	if (!res) {
		return Error("cannot retrieve DRM resources");
	}

	/* iterate all connectors */
	for (int i = 0; i < res->count_connectors; ++i) {
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);

		if (!conn) {
			Error("cannot retrieve DRM connector %u:%u",
			      i, res->connectors[i]).Print();
			continue;
		}
		auto dev = ModesetDev::create(modesets, fd, res, conn);
		if (dev.HasError()) {
			if (dev.GetError().GetErrno() != ENOENT) {
				Error(dev.GetError(), "cannot setup device for connector %u:%u",
				      i,  res->connectors[i]).Print();
			}
			drmModeFreeConnector(conn);
			continue;
		}
		drmModeFreeConnector(conn);
		modesets.push_back(dev.Get());
	}

	/* free resources again */
	drmModeFreeResources(res);
	return Result::Success();
}

void GraphicsHandler::Cleanup() {
	for (auto& iter : modesets) {
		/* restore saved CRTC configuration */
		drmModeSetCrtc(fd,
		               iter.savedCrtc->crtc_id,
		               iter.savedCrtc->buffer_id,
		               iter.savedCrtc->x,
		               iter.savedCrtc->y,
		               &iter.connId,
		               1,
		               &iter.savedCrtc->mode);
		drmModeFreeCrtc(iter.savedCrtc);

		iter.bufs[1].DestroyFb(fd);
		iter.bufs[0].DestroyFb(fd);
	}
}

} // namespace kmswm

