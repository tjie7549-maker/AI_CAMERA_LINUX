.DEFAULT_GOAL := help

help:
	@echo "Targets: doctor build test package"
	@echo "Optional: ENV=configs/build.env BOARD_ENV=configs/board.env VERSION=v0.1.0"

doctor:
	./tools/doctor.sh $(if $(ENV),--env $(ENV))

build:
	./scripts/build_app.sh $(if $(ENV),--env $(ENV))

test:
	./tests/check_scripts.sh
	python3 ./tests/test_config_contract.py
	$(MAKE) -C rv1106-sender/media-sender SDK_DIR=$${SDK_DIR:-/home/summary/linux/luckfox-pico} tracker-test

package:
	./scripts/package_release.sh $(if $(VERSION),--version $(VERSION))

.PHONY: help doctor build test package
