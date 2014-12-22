SUMMARY = "Weston, a Wayland compositor"
DESCRIPTION = "Weston is the reference implementation of a Wayland compositor"
HOMEPAGE = "http://wayland.freedesktop.org"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://COPYING;md5=275efac2559a224527bd4fd593d38466 \
                    file://src/compositor.c;endline=23;md5=aa98a8db03480fe7d500d0b1f4b8850c"

ADIT_SOURCE_GIT = "${BUILD_DIR}/weston"
S= "${ADIT_SOURCE_GIT}"

FILESEXTRAPATHS_prepend := "${THISDIR}/${PN}:"

SRC_URI_append_mx6q += " \
  file://0001-ENGR00314805-2-Add-Vivante-GAL2D-support.patch \
  file://0002-Fix-gal2d-renderer.patch \
  file://0003-Add-a-gal2d-compositor.patch \
"

inherit adit-gitpkgv

PKGV="${@adit_get_git_pkgv(d, '${ADIT_SOURCE_GIT}' )}"
PR = ""

inherit autotools pkgconfig useradd

DEPENDS = "libxkbcommon gdk-pixbuf pixman cairo glib-2.0 jpeg"
DEPENDS += "wayland virtual/egl pango "

RDEPENDS_${PN} += "xkeyboard-config"
RRECOMMENDS_${PN} = "liberation-fonts"

PACKAGES =+ "${PN}-examples"

FILES_${PN}-examples = " \
  ${bindir}/weston-calibrator \
  ${bindir}/weston-clickdot \
  ${bindir}/weston-cliptest \
  ${bindir}/weston-dnd \
  ${bindir}/weston-editor \
  ${bindir}/weston-eventdemo \
  ${bindir}/weston-flower \
  ${bindir}/weston-fullscreen \
  ${bindir}/weston-image \
  ${bindir}/weston-multi-resource \
  ${bindir}/weston-resizor \
  ${bindir}/weston-scaler \
  ${bindir}/weston-simple-damage \
  ${bindir}/weston-simple-egl \
  ${bindir}/weston-simple-keyboard-binding \
  ${bindir}/weston-simple-shm \
  ${bindir}/weston-simple-touch \
  ${bindir}/weston-smoke \
  ${bindir}/weston-stacking \
  ${bindir}/weston-subsurfaces \
  ${bindir}/weston-transformed \
"

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM_${PN} = "--system weston-launch"

EXTRA_OEMAKE_append = " \
  libexecdir="/usr/lib/weston" \
  COMPOSITOR_LIBS="-lGLESv2 -lEGL -lwayland-server -lxkbcommon -lpixman-1" \
  COMPOSITOR_CFLAGS="-I ${STAGING_DIR_HOST}/usr/include/pixman-1 -DLINUX=1 -DEGL_API_FB -DEGL_API_WL" \
  FB_COMPOSITOR_CFLAGS="-DLINUX=1 -DEGL_API_FB -DEGL_API_WL -I $WLD/include" \
  FB_COMPOSITOR_LIBS="-lGLESv2 -lEGL -lwayland-server -lxkbcommon" \
  GAL2D_COMPOSITOR_LIBS="-lGAL -ludev -lmtdev" \
  SIMPLE_EGL_CLIENT_CFLAGS="-DLINUX -DEGL_API_FB -DEGL_API_WL" \
"

EXTRA_OECONF_append = " \
  --enable-clients \
  --enable-demo-clients-install \
  --enable-setuid-install \
  --enable-simple-clients \
  --enable-simple-egl-clients \
  --disable-libunwind \
  --disable-rdp-compositor \
  --disable-rpi-compositor \
  --disable-xwayland \
  --disable-xwayland-test \
"

EXTRA_OECONF_append_mx6q += " \
  WESTON_NATIVE_BACKEND=gal2d-backend.so \
"

EXTRA_OECONF_append_oracle-virtualbox += " \
  WESTON_NATIVE_BACKEND=fbdev-backend.so \
"

EXTRA_OECONF_append_baytrail += " \
  WESTON_NATIVE_BACKEND=fbdev-backend.so \
"

PACKAGECONFIG_mx6q = "imx6"
PACKAGECONFIG_oracle-virtualbox = "x86 fbdev"
PACKAGECONFIG_baytrail = "x86 fbdev"

#
# Compositor choices
#
# Weston on KMS
PACKAGECONFIG[x86] = "--enable-drm-compositor,--disable-drm-compositor,drm udev mesa mtdev"
# Weston on imx6
PACKAGECONFIG[imx6] = "--enable-gal2d-compositor,--disable-gal2d-compositor,udev mtdev"
# Weston on wayland
PACKAGECONFIG[wayland] = "--enable-wayland-compositor,--disable-wayland-compositor,mesa"
# Weston on X11
PACKAGECONFIG[x11] = "--enable-x11-compositor,--disable-x11-compositor,virtual/libx11 libxcb libxcursor cairo"
# Headless Weston
PACKAGECONFIG[headless] = "--enable-headless-compositor,--disable-headless-compositor"
# Weston on framebuffer
PACKAGECONFIG[fbdev] = "--enable-fbdev-compositor,--disable-fbdev-compositor,udev mtdev"
# weston-launch
PACKAGECONFIG[launch] = "--enable-weston-launch,--disable-weston-launch,libpam"
# Weston with libinput backend
PACKAGECONFIG[libinput] = "--enable-libinput-backend,--disable-libinput-backend,libinput"

do_configure_prepend() {
        cp -v ${STAGING_DIR_NATIVE}/${libdir}/pkgconfig/wayland-scanner.pc ${STAGING_DIR_TARGET}/${libdir}/pkgconfig/
}

do_install_append() {
        # Weston doesn't need the .la files to load modules, so wipe them
        rm -vf ${D}/${libdir}/weston/*.la
        rm -v ${STAGING_DIR_TARGET}/${libdir}/pkgconfig/wayland-scanner.pc
}

