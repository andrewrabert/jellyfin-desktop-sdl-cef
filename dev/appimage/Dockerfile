FROM archlinux:latest

RUN pacman -Syu --noconfirm && \
    pacman -S --noconfirm base-devel git sudo fakeroot

# Create build user
RUN useradd -m builder && \
    echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

USER builder
WORKDIR /home/builder

# Install yay
RUN git clone https://aur.archlinux.org/yay-bin.git && \
    cd yay-bin && makepkg -si --noconfirm && \
    cd .. && rm -rf yay-bin

# Install the package
RUN yay -S --noconfirm jellyfin-desktop-cef-git

USER root
WORKDIR /opt/appimage

# Copy the built files to output
CMD cp -r /opt/jellyfin-desktop-cef /output/ && \
    cp /usr/share/applications/jellyfin-desktop-cef.desktop /output/ && \
    cp /usr/share/icons/hicolor/scalable/apps/jellyfin-desktop-cef.svg /output/ 2>/dev/null || true
