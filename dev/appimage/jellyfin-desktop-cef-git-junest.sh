#!/usr/bin/env bash

APP=jellyfin-desktop-cef-git
BIN="jellyfin-desktop-cef" #CHANGE THIS IF THE NAME OF THE BINARY IS DIFFERENT FROM "$APP" (for example, the binary of "obs-studio" is "obs")
DEPENDENCES="" #SYNTAX: "APP1 APP2 APP3 APP4...", LEAVE BLANK IF NO OTHER DEPENDENCIES ARE NEEDED
BASICSTUFF="binutils debugedit gzip"
COMPILERS="base-devel"

# Set keywords to searchan include in names of directories and files in /usr/bin (BINSAVED), /usr/share (SHARESAVED) and /usr/lib (LIBSAVED)
BINSAVED="SAVEBINSPLEASE"
SHARESAVED="SAVESHAREPLEASE"
LIBSAVED="SAVELIBSPLEASE"

# Set the items you want to manually REMOVE. Complete the path in /etc/, /usr/bin/, /usr/lib/, /usr/lib/python*/ and /usr/share/ respectively.
# The "rm" command will take into account the listed object/path and add an asterisk at the end, completing the path to be removed.
# Some keywords and paths are already set. Remove them if you consider them necessary for the AppImage to function properly.
ETC_REMOVED="makepkg.conf pacman"
BIN_REMOVED="gcc"
LIB_REMOVED="gcc"
PYTHON_REMOVED="__pycache__/"
SHARE_REMOVED="gcc icons/AdwaitaLegacy icons/Adwaita/cursors/ terminfo"

# Set mountpoints, they are ment to be set into the AppRun.
# Default mounted files are /etc/resolv.conf, /etc/hosts, /etc/nsswitch.conf, /etc/passwd, /etc/group, /etc/machine-id, /etc/asound.conf and /etc/localtime
# Default mounted directories are /media, /mnt, /opt, /run/media, /usr/lib/locale, /usr/share/fonts, /usr/share/themes, /var, and Nvidia-related directories
# Do not touch this if you are not sure.
mountpoint_files=""
mountpoint_dirs="/usr/share/vulkan"

# Post-installation processes (add whatever you want)
_post_installation_processes() {
	printf "\n◆ User's processes: \n\n"
	echo " - None"
	# Add here your code
}

##########################################################################################################################################################
#	SETUP THE ENVIRONMENT
##########################################################################################################################################################

# Download archimage-builder.sh
if [ ! -f ./archimage-builder.sh ]; then
	ARCHIMAGE_BUILDER="https://raw.githubusercontent.com/ivan-hc/ArchImage/refs/heads/main/core/archimage-builder.sh"
	wget --retry-connrefused --tries=30 "$ARCHIMAGE_BUILDER" -O ./archimage-builder.sh || exit 0
fi

# Create and enter the AppDir
mkdir -p AppDir archlinux && cd archlinux || exit 1

_JUNEST_CMD() {
	# Use proot mode for running commands (first arg not "setup")
	if [ "$1" = "setup" ]; then
		./.local/share/junest/bin/junest "$@"
	else
		./.local/share/junest/bin/junest proot "$@"
	fi
}

# Set archlinux as a temporary $HOME directory (pwd is archlinux/ after cd above)
HOME="$(pwd)"
export XDG_CACHE_HOME="$HOME/.cache"
mkdir -p "$XDG_CACHE_HOME"

##########################################################################################################################################################
#	DOWNLOAD, INSTALL AND CONFIGURE JUNEST
##########################################################################################################################################################

_enable_archlinuxcn() {	ARCHLINUXCN_ON="1"; }
_enable_chaoticaur() { CHAOTICAUR_ON="1"; }
_enable_multilib() { MULTILIB_ON="1"; }

#_enable_archlinuxcn
#_enable_chaoticaur
#_enable_multilib

[ -f ../archimage-builder.sh ] && source ../archimage-builder.sh junest-setup "$@"

##########################################################################################################################################################
#	INSTALL PROGRAMS USING YAY
##########################################################################################################################################################

[ -f ../archimage-builder.sh ] && source ../archimage-builder.sh install "$@"

cd ..

##########################################################################################################################################################
#	APPDIR
##########################################################################################################################################################

[ -f ./archimage-builder.sh ] && source ./archimage-builder.sh appdir "$@"

##########################################################################################################################################################
#	APPRUN
##########################################################################################################################################################

rm -f AppDir/AppRun

# Set to "1" if you want to add Nvidia drivers manager in the AppRun
export NVIDIA_ON=1

[ -f ./archimage-builder.sh ] && source ./archimage-builder.sh apprun "$@"

# AppRun footer, here you can add options and change the way the AppImage interacts with its internal structure
cat <<-'HEREDOC' >> AppDir/AppRun

EXEC=$(grep -e '^Exec=.*' "${HERE}"/*.desktop | head -n 1 | cut -d "=" -f 2- | sed -e 's|%.||g')
if ! echo "$EXEC" | grep -q "/usr/bin"; then EXEC="/usr/bin/$EXEC"; fi

_JUNEST_CMD -- $EXEC "$@"

HEREDOC
chmod a+x AppDir/AppRun

##########################################################################################################################################################
#	COMPILE
##########################################################################################################################################################

[ -f ./archimage-builder.sh ] && source ./archimage-builder.sh compile "$@"

##########################################################################################################################################################
#	CREATE THE APPIMAGE
##########################################################################################################################################################

if test -f ./*.AppImage; then rm -Rf ./*archimage*.AppImage; fi

APPNAME=$(cat AppDir/*.desktop | grep '^Name=' | head -1 | cut -c 6- | sed 's/ /-/g')
REPO="$APPNAME-appimage"
TAG="latest"
UPINFO="gh-releases-zsync|$GITHUB_REPOSITORY_OWNER|$REPO|$TAG|*x86_64.AppImage.zsync"

echo "$VERSION" > ./version

_appimagetool() {
	if ! command -v appimagetool 1>/dev/null; then
		if [ ! -f ./appimagetool ]; then
			echo " Downloading appimagetool..." && curl -#Lo appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-"$ARCH".AppImage && chmod a+x ./appimagetool || exit 1
		fi
		./appimagetool "$@"
	else
		appimagetool "$@"
	fi
}

ARCH=x86_64 _appimagetool -u "$UPINFO" AppDir "$APPNAME"_"$VERSION"-"$ARCHIMAGE_VERSION"-x86_64.AppImage
