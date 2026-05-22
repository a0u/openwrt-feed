#
# Copyright (C) 2026 Albert Ou <aou@eecs.berkeley.edu>
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=wakeonlan-native
PKG_VERSION:=0.42
PKG_RELEASE:=1

PKG_MAINTAINER:=Albert Ou <aou@eecs.berkeley.edu>
PKG_LICENSE:=BSD-3-Clause

include $(INCLUDE_DIR)/package.mk

define Package/wakeonlan-native
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Lightweight replacement for the wakeonlan utility
  PKGARCH:=all
endef

define Package/wakeonlan-native/description
  With this package you can remotely wake up and power on machines which have
  motherboards or network cards that support 'Wake-on-Lan' packets.

  The tool allows you to wake up a single machine, or a group of machines.

  You need the MAC addresses of machines to construct the WOL packets, but,
  in contrast to 'etherwake', you do not need root privileges to use the
  program itself as UDP packets are used.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

TARGET_CFLAGS += -pedantic -Wall -Werror

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -o $(PKG_BUILD_DIR)/wakeonlan wakeonlan.c $(TARGET_LDFLAGS)
endef

define Package/wakeonlan-native/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/wakeonlan $(1)/usr/bin/
endef

$(eval $(call BuildPackage,wakeonlan-native))
