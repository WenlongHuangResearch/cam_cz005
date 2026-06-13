APP_DIR := apps/stereo-recorder

.PHONY: all ci clean stereo-recorder

all: stereo-recorder

stereo-recorder:
	$(MAKE) -C $(APP_DIR) all

ci:
	$(MAKE) -C $(APP_DIR) ci

clean:
	$(MAKE) -C $(APP_DIR) clean
