default: build

# Configure and build the CLI and GTK GUI.
build *options:
    make build {{ options }}

# Configure the CMake build tree without compiling.
configure *options:
    make configure {{ options }}

# Build and run the test suite.
test *options:
    make test {{ options }}

# Build and install under PREFIX (defaults to /usr; usually requires root).
install *options:
    sudo make install {{ options }}

# Build and install under ~/.local without root privileges.
install-user *options:
    make install-user {{ options }}

# Remove the configured build directory.
clean *options:
    make clean {{ options }}

# Build only the command-line application.
cli *options:
    make build GUI=OFF {{ options }}

# Create a debug build.
debug *options:
    make build BUILD_TYPE=Debug {{ options }}
