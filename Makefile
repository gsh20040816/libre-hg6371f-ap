-include local.mk

AP_HOST ?= root@192.168.1.2
AP_BASE ?= /opt/upt/apps/ap-panel
BUILD_HOST ?= local
BASE_ROOTFS ?= backups/rootfs2.bin
OUT_DIR ?= build
ROOTFS_OUT ?= $(OUT_DIR)/rootfs-custom.sqfs
ALPINE_OUT ?= $(OUT_DIR)/rootfs-alpine.sqfs

export COPYFILE_DISABLE=1

.PHONY: help build-bcmopen test-bcmopen vm-test-bcmopen diff-bcmopen build-server build-mtd-wp build-rootfs build-alpine-rootfs deploy-web deploy-server deploy-scripts deploy-rootfs deploy-all check

help:
	@echo "Targets:"
	@echo "  build-bcmopen        Cross-compile libbcmopen.a & static bcmctl via zig cc"
	@echo "  test-bcmopen         Run host C unit tests for bcmopen"
	@echo "  vm-test-bcmopen      Run offline local QEMU ARMv7 syscall-mock diff suite"
	@echo "  diff-bcmopen         Run live AP side-by-side regression diff vs /opt/bcm"
	@echo "  build-server         Cross-compile ap-panel-srv for BCM68252 (linux/arm GOARM=7)"
	@echo "  build-mtd-wp         Cross-compile fh-mtd-wp (mtd2 write-protect toggle)"
	@echo "  build-rootfs         Pack & validate stock-based rootfs on $(BUILD_HOST) -> $(ROOTFS_OUT)"
	@echo "  build-alpine-rootfs  Pack & validate Alpine armv7 + open-source bcmctl rootfs -> $(ALPINE_OUT)"
	@echo "  deploy-web      Hot-deploy frontend assets to AP (no restart needed)"
	@echo "  deploy-server   Build and deploy ap-panel-srv binary to AP"
	@echo "  deploy-scripts  Deploy device/start.sh and device/tune.sh to AP"
	@echo "  deploy-rootfs   Validate, flash inactive slot, one-shot reboot, verify & commit (IMG=...)"
	@echo "  deploy-all      Deploy web, server binary, and device scripts"
	@echo "  check           Syntax-check JS, Go build, and shell scripts"

build-bcmopen:
	$(MAKE) -C bcmopen OUT_DIR="$(abspath $(OUT_DIR))" all

test-bcmopen:
	$(MAKE) -C bcmopen OUT_DIR="$(abspath $(OUT_DIR))" test

vm-test-bcmopen: build-bcmopen
	python3 bcmopen/tools/vm_test.py

diff-bcmopen: build-bcmopen
	python3 bcmopen/tools/ap_diff.py --host "$(AP_HOST)"

build-server: build-bcmopen
	@mkdir -p "$(OUT_DIR)"
	cd panel/server && GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=1 \
		CC="zig cc -target arm-linux-musleabihf -mcpu=cortex_a7" \
		CGO_LDFLAGS="-L$(abspath $(OUT_DIR))" \
		go build -trimpath -tags netgo,osusergo -ldflags="-s -w -linkmode external -extldflags '-static -s'" -o "$(abspath $(OUT_DIR))/ap-panel-srv" .

build-mtd-wp:
	@mkdir -p $(OUT_DIR)
	GOOS=linux GOARCH=arm GOARM=7 CGO_ENABLED=0 \
		go build -trimpath -ldflags="-s -w" -o $(OUT_DIR)/fh-mtd-wp firmware/fh-mtd-wp.go

build-rootfs:
	@test -f "$(BASE_ROOTFS)" || { echo "missing base rootfs: $(BASE_ROOTFS)" >&2; exit 2; }
	@mkdir -p "$(dir $(ROOTFS_OUT))"
	@if [ "$(BUILD_HOST)" = "local" ]; then \
		sh firmware/build-rootfs.sh "$(BASE_ROOTFS)" "$(ROOTFS_OUT)" && \
		python3 firmware/deploy-rootfs.py "$(ROOTFS_OUT)" --check-only; \
	else \
		RDIR=$$(ssh -o BatchMode=yes $(BUILD_HOST) 'mktemp -d /tmp/hg6371f-build.XXXXXX') && \
		trap 'ssh -o BatchMode=yes $(BUILD_HOST) "rm -rf '\''$$RDIR'\''" >/dev/null 2>&1 || true' EXIT && \
		tar --format ustar -cf - firmware/build-rootfs.sh firmware/deploy-rootfs.py firmware/rootfs-overlay \
			| ssh -o BatchMode=yes $(BUILD_HOST) "tar -xf - -C '$$RDIR'" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "cat > '$$RDIR/base.bin'" < "$(BASE_ROOTFS)" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "sh '$$RDIR/firmware/build-rootfs.sh' '$$RDIR/base.bin' '$$RDIR/out.sqfs' && python3 '$$RDIR/firmware/deploy-rootfs.py' '$$RDIR/out.sqfs' --check-only" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "cat '$$RDIR/out.sqfs'" > "$(ROOTFS_OUT).tmp" && \
		mv "$(ROOTFS_OUT).tmp" "$(ROOTFS_OUT)"; \
	fi
	@echo "saved $(ROOTFS_OUT)"

build-alpine-rootfs: build-bcmopen
	@test -f "$(BASE_ROOTFS)" || { echo "missing base rootfs: $(BASE_ROOTFS)" >&2; exit 2; }
	@test -f "$(OUT_DIR)/bcmctl" || { echo "missing bcmctl binary: $(OUT_DIR)/bcmctl" >&2; exit 2; }
	@mkdir -p "$(dir $(ALPINE_OUT))"
	@if [ "$(BUILD_HOST)" = "local" ]; then \
		sh firmware/alpine/build-alpine-rootfs.sh "$(BASE_ROOTFS)" "$(ALPINE_OUT)" "$(OUT_DIR)/bcmctl" && \
		python3 firmware/deploy-rootfs.py "$(ALPINE_OUT)" --check-only; \
	else \
		RDIR=$$(ssh -o BatchMode=yes $(BUILD_HOST) 'mktemp -d /tmp/hg6371f-alpine-build.XXXXXX') && \
		trap 'ssh -o BatchMode=yes $(BUILD_HOST) "rm -rf '\''$$RDIR'\''" >/dev/null 2>&1 || true' EXIT && \
		tar --format ustar -cf - firmware/deploy-rootfs.py firmware/alpine \
			| ssh -o BatchMode=yes $(BUILD_HOST) "tar -xf - -C '$$RDIR'" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "cat > '$$RDIR/bcmctl' && chmod 755 '$$RDIR/bcmctl'" < "$(OUT_DIR)/bcmctl" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "cat > '$$RDIR/base.bin'" < "$(BASE_ROOTFS)" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "sh '$$RDIR/firmware/alpine/build-alpine-rootfs.sh' '$$RDIR/base.bin' '$$RDIR/out.sqfs' '$$RDIR/bcmctl' && python3 '$$RDIR/firmware/deploy-rootfs.py' '$$RDIR/out.sqfs' --check-only" && \
		ssh -o BatchMode=yes $(BUILD_HOST) "cat '$$RDIR/out.sqfs'" > "$(ALPINE_OUT).tmp" && \
		mv "$(ALPINE_OUT).tmp" "$(ALPINE_OUT)"; \
	fi
	@echo "saved $(ALPINE_OUT)"

deploy-web:
	node --check panel/web/app.js
	@SUMS=$$(cd panel/web && (command -v sha256sum >/dev/null 2>&1 && sha256sum index.html style.css app.js || shasum -a 256 index.html style.css app.js)); \
	tar --format ustar -czf - -C panel/web index.html style.css app.js \
		| ssh -o BatchMode=yes $(AP_HOST) "set -eu; mkdir -p $(AP_BASE)/web; T=\$$(mktemp -d $(AP_BASE)/.deploy-web.XXXXXX); trap 'rm -rf \"\$$T\"' EXIT; tar -xzof - -C \"\$$T\"; (cd \"\$$T\" && printf '%s\n' '$$SUMS' | sha256sum -c - >/dev/null); rm -f $(AP_BASE)/web/vendor-methods.json; mv -f \"\$$T\"/index.html \"\$$T\"/style.css \"\$$T\"/app.js $(AP_BASE)/web/; sync"

deploy-server: build-server
	@SUM=$$(cd $(OUT_DIR) && (command -v sha256sum >/dev/null 2>&1 && sha256sum ap-panel-srv || shasum -a 256 ap-panel-srv)); \
	tar --format ustar -czf - -C $(OUT_DIR) ap-panel-srv \
		| ssh -o BatchMode=yes $(AP_HOST) "set -eu; mkdir -p $(AP_BASE)/bin; T=\$$(mktemp -d $(AP_BASE)/.deploy-bin.XXXXXX); trap 'rm -rf \"\$$T\"' EXIT; tar -xzof - -C \"\$$T\"; (cd \"\$$T\" && printf '%s\n' '$$SUM' | sha256sum -c - >/dev/null); chmod 755 \"\$$T/ap-panel-srv\"; mv -f \"\$$T/ap-panel-srv\" $(AP_BASE)/bin/ap-panel-srv; sync; if command -v rc-service >/dev/null 2>&1; then rc-service ap-panel restart && rc-service ap-panel status; elif [ -x $(AP_BASE)/start.sh ]; then setsid sh $(AP_BASE)/start.sh >/dev/null 2>&1; fi"

deploy-scripts:
	@SUMS=$$(cd device && (command -v sha256sum >/dev/null 2>&1 && sha256sum start.sh tune.sh || shasum -a 256 start.sh tune.sh)); \
	tar -czf - -C device start.sh tune.sh \
		| ssh -o BatchMode=yes $(AP_HOST) "set -eu; mkdir -p $(AP_BASE); T=\$$(mktemp -d $(AP_BASE)/.deploy-scr.XXXXXX); trap 'rm -rf \"\$$T\"' EXIT; tar -xzof - -C \"\$$T\"; (cd \"\$$T\" && printf '%s\n' '$$SUMS' | sha256sum -c - >/dev/null); chmod 755 \"\$$T/start.sh\" \"\$$T/tune.sh\"; mv -f \"\$$T/start.sh\" \"\$$T/tune.sh\" $(AP_BASE)/; sync"

deploy-rootfs:
	@test -n "$(IMG)" || { echo "usage: make deploy-rootfs IMG=/path/to/rootfs.sqfs [BOOTFS=/path/to/bootfs.bin]" >&2; exit 2; }
	python3 firmware/deploy-rootfs.py "$(IMG)" --host "$(AP_HOST)" $(if $(BOOTFS),--bootfs "$(BOOTFS)",)

deploy-all: deploy-web deploy-server

check:
	node --check panel/web/app.js
	sh -n device/start.sh device/tune.sh firmware/build-rootfs.sh firmware/flash-ab.sh firmware/rootfs-overlay/usr/sbin/ap-boot-hook.sh firmware/rootfs-overlay/fhrom/fhshell/process_check.sh firmware/alpine/build-alpine-rootfs.sh firmware/alpine/rootfs/usr/local/sbin/bcm-early-mount firmware/alpine/rootfs/usr/local/sbin/bcm-rf firmware/alpine/rootfs/usr/local/sbin/ap-rollback-guard firmware/alpine/rootfs/usr/local/sbin/ap-led firmware/alpine/rootfs/etc/init.d/ap-led firmware/alpine/rootfs/etc/init.d/bcm-hw firmware/alpine/rootfs/etc/init.d/ap-panel firmware/alpine/rootfs/etc/network/if-pre-up.d/z-brmac
	python3 -m py_compile recovery/fh_shell.py recovery/ap_watchdog.py firmware/deploy-rootfs.py
	cd panel/server && go vet ./...
	go vet firmware/fh-mtd-wp.go
