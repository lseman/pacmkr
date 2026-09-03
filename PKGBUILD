# Maintainer: Laio Seman <laioseman@gmail.com>

pkgname=pacmkr
_pkgname=pacmkr
pkgver=0.1.0
pkgrel=1
pkgdesc='A fast, modern package manager and AUR helper for Arch Linux'
arch=('x86_64' 'aarch64')
url='https://github.com/lseman/pacmkr'
license=('MIT')
depends=('openssl' 'libalpm' 'gcc-libs')
makedeps=('cmake' 'gcc' 'fakeroot' 'bsdtar' 'zstd' 'nlohmann-json')
optdepends=('gtkmm-4.0: GTK4 desktop dashboard (pacmkr-gui)')
provides=("${_pkgname}")
conflicts=()
source=("git+https://github.com/lseman/pacmkr.git#tag=v${pkgver}")
sha256sums=('SKIP')  # Update with actual checksum after first build

build() {
    cd "${_pkgname}"
    
    cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DPACMKR_BUILD_TESTS=ON \
        -DPACMKR_BUILD_GTK_GUI=OFF \
        -DPACMKR_USE_OPENSSL=ON
    
    cmake --build build --parallel "$(nproc)"
}

check() {
    cd "${_pkgname}"
    
    ctest --test-dir build --output-on-failure -j "$(nproc)"
}

package() {
    cd "${_pkgname}"
    
    # CMake install handles most of this, but we need to ensure proper paths
    cmake --install build \
        --prefix "${pkgdir}/usr"
    
    # Install configuration example
    install -Dm644 config.example "${pkgdir}/etc/xdg/pacmkr/config.example"
    
    # Install README and LICENSE
    install -Dm644 README.md "${pkgdir}/usr/share/doc/${_pkgname}/README.md"
    install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${_pkgname}/LICENSE"
}

# Generate checksums after first successful build:
# makepkg --printsrcinfo > .SRCINFO
