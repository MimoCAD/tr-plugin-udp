# Status UDP Plugin
You should clone trunk-recorder first.
Configure `cd ~/trunk-build/`, `cmake ../trunk-recorder`, build `make -j4` and install `make install`. (This gives you the required header files in their correct location.)
Then `cd` into `/trunk-recorder/user_plugins/`
Then git clone this repository into that directory.
It should create a subdirectory named `tr-plugin-udp`.
With this repo in the correct directory, it will be built automatically along side `trunk-recorder` thanks to some CMake magic.
Now wehn you do `cmake ../trunk-recorder` it will pickup this plugin. Build and install as you did before. `make -j4` and `make install`.
