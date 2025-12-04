# Status UDP Plugin
* Clone and prepare Trunk Recorder First.
  * `cd ~`
  * `git clone https://github.com/TrunkRecorder/trunk-recorder.git`
  * `mkdir trunk-build`
* Clone this repo into `~/trunk-recorder/user_plugins/tr-plugin-udp`.
  * `git clone https://github.com/MimoCAD/tr-plugin-udp.git ~/trunk-recorder/user_plugins/tr-plugin-udp`
* Configure & Build Trunk Recorder
  * `cd ~/trunk-build/`
  * `cmake ../trunk-recorder`
  * `make -j4`
