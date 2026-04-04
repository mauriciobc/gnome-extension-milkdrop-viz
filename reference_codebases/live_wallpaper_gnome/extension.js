import Clutter from "gi://Clutter";
import GLib from "gi://GLib";
import { Extension } from "resource:///org/gnome/shell/extensions/extension.js";
import * as Main from "resource:///org/gnome/shell/ui/main.js";
import Gst from "gi://Gst";
import St from "gi://St";

// Initialize GStreamer
Gst.init(null);

export default class VideoWallpaperExtension extends Extension {
  // El constructor es la función 'init()' de la sintaxis antigua
  constructor(metadata) {
    super(metadata);
    this.player = null;
    this.widget = null;
    this.timeoutId = null;
  }

  enable() {
    try {
      this.timeoutId = GLib.timeout_add_seconds(
        GLib.PRIORITY_DEFAULT,
        5,
        () => {
          let file = GLib.build_filenamev([
            GLib.get_user_special_dir(GLib.UserDirectory.DIRECTORY_VIDEOS),
            "video_test.mp4",
          ]);
          let uri = GLib.filename_to_uri(file, null);

          this.player = Gst.ElementFactory.make("playbin", null);
          const sink = Gst.ElementFactory.make("cluttersink", null);

          if (!sink) {
            throw new Error(
              "ClutterSink element not found. Make sure the 'clutter-gst' GStreamer plugin is installed."
            );
          }

          this.player.set_property("video-sink", sink);
          this.player.set_property("uri", uri);

          this.widget = new St.Widget({
            layout_manager: new Clutter.BinLayout(),
          });
          sink.set_property("widget", this.widget);

          // Set the size and position of the video
          let monitor = Main.layoutManager.primaryMonitor;
          this.widget.set_size(monitor.width, monitor.height);
          this.widget.set_position(monitor.x, monitor.y);

          // Add the widget to the background group
          Main.layoutManager._backgroundGroup.add_child(this.widget);

          // Loop the video
          this.player.connect("about-to-finish", () => {
            this.player.seek_simple(Gst.Format.TIME, Gst.SeekFlags.FLUSH, 0);
          });

          this.player.set_state(Gst.State.PLAYING);
          this.timeoutId = null;
          return GLib.SOURCE_REMOVE;
        }
      );
    } catch (e) {
      const logFile = GLib.build_filenamev([this.path, "error.log"]);
      GLib.file_set_contents(
        logFile,
        new Date().toISOString() + " - " + e.message + "\n"
      );
      this.disable();
    }
  }

  disable() {
    if (this.timeoutId) {
      GLib.source_remove(this.timeoutId);
      this.timeoutId = null;
    }

    if (this.player) {
      this.player.set_state(Gst.State.NULL);
      this.player = null;
    }
    if (this.widget) {
      Main.layoutManager._backgroundGroup.remove_child(this.widget);
      this.widget.destroy();
      this.widget = null;
    }
  }
}
