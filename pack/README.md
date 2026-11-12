# spider 打包

`pack/` 以同一份 rootfs 产出 DEB、RPM 和 Gentoo overlay，三种格式内容一致。

## 依赖

构建机需要：

- CMake、make、C 编译器、strip
- Debian 包：`dpkg-deb`
- RPM 包：`rpmbuild`
- Gentoo overlay：`tar`、`gzip`
- Python 3（changelog 转换）

项目自身的构建依赖由 CMake 检查：libcurl、sqlite3、json-c、OpenSSL。
第三方 lexbor 和 civetweb 已随源码提供，不需要联网。

## 构建

```sh
make -C pack              # 三种格式全部构建
make -C pack deb          # 仅 DEB
make -C pack rpm          # 仅 RPM
make -C pack emerge       # 仅 Gentoo overlay
make -C pack rootfs       # 只生成 build/rootfs
make -C pack test         # 构建并运行 ctest（不参与默认出包流程）
make -C pack clean
make -C pack distclean
```

版本号取自根目录 `CMakeLists.txt` 的 `project(spider VERSION ...)`。
架构自动映射：`x86_64 → amd64`，`aarch64 → arm64`。

产物位于 `pack/dist/`：

- `spider-crawler-<version>-<arch>.deb`
- `spider-crawler-<version>-<arch>.rpm`
- `spider-crawler-gentoo-overlay-<arch>-<version>.tar.gz`

## 安装布局

| 路径 | 说明 |
|---|---|
| `/opt/spider/bin/spider` | 主程序 |
| `/opt/spider/share/spider/ui/` | Web 看板静态资源 |
| `/usr/bin/spider` | 指向主程序的符号链接 |
| `/usr/lib/systemd/system/spider.service` | systemd 服务 |
| `/var/lib/spider` | 数据目录（SQLite、下载文件） |

`spider` 会从可执行文件旁的 `ui/` 或 `bin/../share/spider/ui` 查找前端，
因此资源放在 `/opt/spider/share/spider/ui`，不需要修改源码。

服务默认参数：

```
--host 127.0.0.1 --port 8080
--data-dir /var/lib/spider
--ui-dir /opt/spider/share/spider/ui
```

服务文件启用了 `ProtectSystem=strict` 和 `ReadWritePaths=/var/lib/spider`，
除数据目录外文件系统只读。需要调整参数时使用 `systemctl edit spider.service`。

安装时创建 `spider` 系统用户/组和 `/var/lib/spider`（0750），执行
`systemctl daemon-reload` 并 `enable` 服务，但不会自动启动。
卸载时停止并禁用服务，保留 `/var/lib/spider` 和用户，避免删除爬虫数据。

## Debian

```sh
sudo dpkg -i pack/dist/spider-crawler-<version>-<arch>.deb
sudo systemctl start spider.service
```

运行时依赖：`libc6, libcurl4, libsqlite3-0, libjson-c5, libssl3`。

## RPM

```sh
sudo rpm -Uvh pack/dist/spider-crawler-<version>-<arch>.rpm
sudo systemctl start spider.service
```

RPM 的 soname 依赖由 rpmbuild 自动生成；安装脚本需要 `shadow-utils`。

## Gentoo overlay

先以普通用户构建 overlay，再以 root 安装（脚本自动生成 Manifest 并调用 emerge）：

```sh
make -C pack emerge
sudo pack/tools/install-gentoo.sh
# 或指定已有的 overlay 包
sudo pack/tools/install-gentoo.sh pack/dist/spider-crawler-gentoo-overlay-<arch>-<version>.tar.gz
# 也可以使用 make 目标（不会重新构建）
sudo make -C pack install-gentoo
```

脚本会解压到临时目录、校验 ebuild、生成 Manifest，然后以
`PORTDIR_OVERLAY` 临时仓库方式执行 `emerge --oneshot app-admin/spider-crawler`，
不会把临时 overlay 留在系统里。

手动安装：

```sh
tar -xzf pack/dist/spider-crawler-gentoo-overlay-<arch>-<version>.tar.gz
cd spider-crawler-gentoo-overlay-<arch>-<version>
ebuild app-admin/spider-crawler/spider-crawler-<version>.ebuild manifest
PORTDIR_OVERLAY="$PWD" emerge --oneshot app-admin/spider-crawler
```

ebuild 使用 `RESTRICT="bindist mirror strip test"`，二进制标记为 `QA_PREBUILT`，
不会在安装时重新编译或 strip。`keepdir` 保留数据目录，`pkg_postinst`（与 DEB/RPM
共用同一脚本）创建 `spider` 用户并修正目录属主；systemd 的
`StateDirectory=spider` 会再次兜底。

## 注意

- 仓库没有 LICENSE 文件，RPM 和 Gentoo 暂时使用 `all-rights-reserved`。
- `HOMEPAGE` 暂为 `https://local/spider`，有正式地址后请修改
  `src/build-emerge.sh` 生成的 ebuild。
- 升级不会覆盖 `/var/lib/spider`；重启服务后新版本才会生效。
