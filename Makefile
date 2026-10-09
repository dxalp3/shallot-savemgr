PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
VERSION ?= 1.13

PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_INCDIR := $(PS5_PAYLOAD_SDK)/target/include
PS5_LIBDIR := $(PS5_PAYLOAD_SDK)/target/lib

CFLAGS := -O2 -Wall -D_BSD_SOURCE -std=gnu11 -Isrc -I$(PS5_INCDIR)
SQLITE_FLAGS := -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_WAL -w
LDFLAGS := -L$(PS5_LIBDIR)
LIBS := -lkernel_sys -lkernel -lSceSystemService -lSceUserService -lSceFsInternalForVsh -lSceRegMgr -lSceNet
I18N_FILES := $(wildcard src/i18n/*.js)
PREVIEW_DIR ?= /tmp/shallot-savemgr-ui-preview
PREVIEW_PORT ?= 8082

all: shallot-savemgr.elf

src/ui.h: src/ui.html src/i18n $(I18N_FILES)
	sed 's/__VERSION__/v$(VERSION)/g' $< > src/_ui_tmp.html
	for file in $(I18N_FILES); do sed -i '/^"__I18N_DICTIONARIES__";$$/r '"$$file" src/_ui_tmp.html; done
	sed -i '/^"__I18N_DICTIONARIES__";$$/d' src/_ui_tmp.html
	xxd -i src/_ui_tmp.html > $@
	sed -i 's/src__ui_tmp_html/src_ui_html/g' $@
	rm -f src/_ui_tmp.html

src/sqlite3.o: src/sqlite3.c
	$(PS5_CC) -O2 -D_BSD_SOURCE -std=gnu11 -I$(PS5_INCDIR) $(SQLITE_FLAGS) -c -o $@ $<

# The PC client, served by /api/pc/shallot_sync.py, its Docker files
# (/api/pc/docker/<name>) and the vault (/api/pc/vault/<name>).
PC_FILES = pc/shallot_sync.py pc/Dockerfile pc/docker-entrypoint.sh pc/docker-compose.yml \
	vault/shallot_vault.py vault/Dockerfile vault/docker-compose.yml vault/Caddyfile
src/pc_client.h: $(PC_FILES)
	rm -f $@
	for f in $(PC_FILES); do xxd -i $$f >> $@; done

# Home-screen tile icon, served to the PS5 app installer.
src/launcher_icon.h: assets/icon0.png
	xxd -i assets/icon0.png > $@

shallot-savemgr.elf: src/main.c src/lansync_core.inc.c src/lansync_crypto.inc.c src/lansync_save.inc.c src/lansync_worker.inc.c src/lansync_blocks.inc.c src/lansync_sync.inc.c src/lansync_reconcile.inc.c src/lansync_restore.inc.c src/lansync_recovery.inc.c src/lansync_system.inc.c src/lansync_pclink.inc.c src/lansync_launcher.inc.c src/lansync_http.inc.c src/lansync_vault.inc.c src/lansync_auth.inc.c src/ui.h src/pc_client.h src/launcher_icon.h src/sqlite3.o
	$(PS5_CC) $(CFLAGS) $(LDFLAGS) -o $@ src/main.c src/sqlite3.o $(LIBS)

clean:
	rm -f shallot-savemgr.elf src/ui.h src/pc_client.h src/launcher_icon.h src/sqlite3.o src/_ui_tmp.html

ui-preview: src/ui.html src/i18n $(I18N_FILES)
	mkdir -p $(PREVIEW_DIR)
	sed 's/__VERSION__/local-preview/g' src/ui.html > $(PREVIEW_DIR)/index.html
	for file in $(I18N_FILES); do sed -i '/^"__I18N_DICTIONARIES__";$$/r '"$$file" $(PREVIEW_DIR)/index.html; done
	sed -i '/^"__I18N_DICTIONARIES__";$$/d' $(PREVIEW_DIR)/index.html
	@echo "UI preview: http://127.0.0.1:$(PREVIEW_PORT)/"
	cd $(PREVIEW_DIR) && python3 -m http.server $(PREVIEW_PORT) --bind 0.0.0.0

.PHONY: all clean ui-preview
