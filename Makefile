# make only accept argument format: xxxx=xxxx, other format will treat as a target.
# add [enable-split-platform enable-debug] to .PHONY can ignore target not exist error.
.PHONY: help build clean enable-split-platform enable-debug enable-gpl use-prebuilt

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  build [arguments]     Build the project for iOS and macOS"
	@echo "                        Arguments:"
	@echo "                            platform=ios,macos        Only build specified platform (ios,macos,tvos,tvsimulator,isimulator,maccatalyst,xros,xrsimulator)"
	@echo "                            libs=libmpv,FFmpeg        Only compile these libraries from source (libass,FFmpeg,libmpv,...)"
	@echo "                            use-prebuilt              Restore self-built libraries outside libs= from the published"
	@echo "                                                      content-addressed binaries recorded in artifacts.json"
	@echo "  clean                 Clean the build artifacts"
	@echo "  help                  Display this help message"

build:
	swift run --build-path ./.build --package-path Sources/BuildScripts build $(filter-out $@,$(MAKECMDGOALS)) $(MAKEFLAGS)

clean:
	@find . -name '.build' -type d -exec rm -rf {} +
	@find . -name '.swiftpm' -type d -exec rm -rf {} +
	@rm -rf ./dist
	@rm -rf ./*.log
	@swift package reset
