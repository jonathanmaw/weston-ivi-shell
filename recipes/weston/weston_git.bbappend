FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"

SRC_URI_mx6q += "file://0001-ENGR00314805-2-Add-Vivante-GAL2D-support.patch \
            		file://0002-Fix-gal2d-renderer.patch \
			file://0003-Add-a-gal2d-compositor.patch"


PACKAGECONFIG_mx6q = "imx6q"
PACKAGECONFIG_oracle-virtualbox = "x86 fbdev"

EXTRA_OECONF_append_mx6q += "\
                --enable-simple-egl-clients \
                --disable-libunwind \
                --disable-xwayland-test \
                WESTON_NATIVE_BACKEND=gal2d-backend.so"

EXTRA_OECONF_oracle-virtualbox += "\
                --enable-simple-egl-clients \
                --disable-libunwind \
                --disable-xwayland-test \
                WESTON_NATIVE_BACKEND=drm-backend.so"

export libexecdir="/usr/lib/weston"
export COMPOSITOR_LIBS="-lGLESv2 -lEGL -lwayland-server -lxkbcommon -lpixman-1"
export COMPOSITOR_CFLAGS="-I ${STAGING_DIR_HOST}/usr/include/pixman-1 -DLINUX=1 -DEGL_API_FB -DEGL_API_WL"
export FB_COMPOSITOR_CFLAGS="-DLINUX=1 -DEGL_API_FB -DEGL_API_WL -I $WLD/include"
export FB_COMPOSITOR_LIBS="-lGLESv2 -lEGL -lwayland-server -lxkbcommon"
export GAL2D_COMPOSITOR_LIBS="-lGAL -ludev -lmtdev"
export SIMPLE_EGL_CLIENT_CFLAGS="-DLINUX -DEGL_API_FB -DEGL_API_WL"


PACKAGE_ARCH_mx6q = "${MACHINE_ARCH}"
