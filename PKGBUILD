# Maintainer: Nash Kaminski <nashkaminski@comcast.net>
# Contributor: Peter Chinetti <peter@chinetti.me>

_pkgbase=amd-gseriessoc-wdt
pkgname=amd-gseriessoc-wdt-dkms
pkgver=1
pkgrel=1
pkgdesc="Watchdog timer drivers for the AMD G series SoC"
arch=('i686' 'x86_64')
url="http://repository.timesys.com/buildsources/a/amd-gseriessoc-wdt/"
license=('GPL2')
depends=('dkms')
conflicts=("${_pkgbase}")
#install=${pkgname}.install
source=('amd_wdt.c' 'amd_wdt.h' 'Makefile' 'dkms.conf')
md5sums=('59d461613b7d8c5d5095df6b9a4b7b67'
         'c1bec0fb06e816550b46d70e49eab52f'
         '1b44403bcf47d1547250619fa8012525'
         '9fddda1dc59112df4ecf76f918a5ebda')

build() {
  msg2 "Starting make..."
  make
}

package() {
  # Install
  msg2 "Starting install..."

  # Copy dkms.conf
  install -Dm644 dkms.conf "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/dkms.conf

  # Set name and version
  sed -e "s/@_PKGBASE@/${_pkgbase}/" \
      -e "s/@PKGVER@/${pkgver}/" \
      -i "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/dkms.conf

  # Copy sources (including Makefile)
  cp -r Makefile "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/
  cp -r *.c "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/
  cp -r *.h "${pkgdir}"/usr/src/${_pkgbase}-${pkgver}/
}
