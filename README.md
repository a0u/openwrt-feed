# OpenWrt custom packages feed

To use these packages, add the following line to `feeds.conf.defaults`
in the OpenWrt buildroot:

```
src-git custom https://github.com/a0u/openwrt-feed.git
```

To install all package definitions, run:

```
./scripts/feeds update custom
./scripts/feeds install -a -p custom
```
