SUMMARY = "Weston, a Wayland compositor"
DESCRIPTION = "Weston is the reference implementation of a Wayland compositor"
HOMEPAGE = "http://wayland.freedesktop.org"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://COPYING;md5=275efac2559a224527bd4fd593d38466 \
                    file://src/compositor.c;endline=23;md5=aa98a8db03480fe7d500d0b1f4b8850c"

ADIT_SOURCE_GIT = "${BUILD_DIR}/weston"
S= "${ADIT_SOURCE_GIT}"

inherit adit-gitpkgv

PKGV="${@adit_get_git_pkgv(d, '${ADIT_SOURCE_GIT}' )}"
PR = ""
DISTRO_PR=""


#SRC_URI = "file://weston.png \
#           file://weston.desktop"
#SRC_URI[md5sum] = "ffe7c3bc0e7eb39a305cbbea8c7766f3"
#SRC_URI[sha256sum] = "f7141334b141ae1a6435bd03bfdb01b7fb628f39259164f201e7e71c8d815bc7"

inherit autotools pkgconfig useradd

DEPENDS = "libxkbcommon gdk-pixbuf pixman cairo glib-2.0 jpeg"
DEPENDS += "wayland virtual/egl pango "

EXTRA_OECONF = "--enable-setuid-install \
                --disable-xwayland \
                --enable-simple-clients \
                --enable-clients \
                --disable-simple-egl-clients \
                --disable-libunwind \
                --disable-rpi-compositor \
		--disable-xwayland-test \
                --disable-rdp-compositor \
		--disable-rpi-compositor"


PACKAGECONFIG ??= "x86 imx6q"
#
# Compositor choices
#
# Weston on KMS
PACKAGECONFIG[x86] = "--enable-drm-compositor,--disable-drm-compositor,drm udev mesa mtdev"
# Weston on framebuffer
PACKAGECONFIG[imx6q] = "--enable-gal2d-compositor, --disable-gal2d-compositor ,udev mtdev"
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
PACKAGECONFIG[libinput] = "--enable-libinput-backend,--disable-libinput-backend,libinput "

#do_configure_prepend_mx6q() {
#        cp ${BUILD_DIR}/oe-*/build/tmp/sysroots/x86_64-linux/usr/lib/pkgconfig/wayland-scanner.pc ${BUILD_DIR}/oe-MGC_20131203-mx6q/build/tmp/sysroots/mx6q/usr/lib/pkgconfig/
#}

#do_configure_prepend_oracle-virtualbox() {
#        cp ${BUILD_DIR}/oe-*/build/tmp/sysroots/x86_64-linux/usr/lib/pkgconfig/wayland-scanner.pc ${BUILD_DIR}/oe-*/build/tmp/sysroots/oracle-virtualbox/usr/lib/pkgconfig/
#}

do_configure_prepend() {
        cp -v ${STAGING_DIR_NATIVE}/${libdir}/pkgconfig/wayland-scanner.pc ${STAGING_DIR_TARGET}/${libdir}/pkgconfig/
}

#do_install_append_mx6q() {
#	# Weston doesn't need the .la files to load modules, so wipe them
#	rm -f ${D}/${libdir}/weston/*.la
#        rm ${BUILD_DIR}/oe-MGC_20131203-mx6q/build/tmp/sysroots/mx6q/usr/lib/pkgconfig/wayland-scanner.pc
#}
#
#do_install_append_oracle-virtualbox() {
#	# Weston doesn't need the .la files to load modules, so wipe them
#	rm -f ${D}/${libdir}/weston/*.la
#        rm ${BUILD_DIR}/oe-*/build/tmp/sysroots/oracle-virtualbox/usr/lib/pkgconfig/wayland-scanner.pc
#}

do_install_append() {
        # Weston doesn't need the .la files to load modules, so wipe them
        rm -vf ${D}/${libdir}/weston/*.la
        rm -v ${STAGING_DIR_TARGET}/${libdir}/pkgconfig/wayland-scanner.pc
}

PACKAGES += "${PN}-examples"

FILES_${PN} = "${bindir}/weston ${bindir}/weston-terminal ${bindir}/weston-info ${bindir}/weston-launch ${bindir}/wcap-decode ${libexecdir} ${datadir}"
FILES_${PN}-examples = "${bindir}/*"

RDEPENDS_${PN} += "xkeyboard-config"
RRECOMMENDS_${PN} = "liberation-fonts"

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM_${PN} = "--system weston-launch"
