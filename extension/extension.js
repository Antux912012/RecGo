// GNOME Shell Extension for RecGo.
// Compatible with GNOME Shell 45, 46, and 47 (ESM).

import GObject from 'gi://GObject';
import St from 'gi://St';
import Gio from 'gi://Gio';
import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';

const RecGoIface = `
<node>
  <interface name="io.github.antonio.RecGo.Control">
    <method name="Start">
      <arg type="b" direction="out" />
    </method>
    <method name="Pause">
      <arg type="b" direction="out" />
    </method>
    <method name="Stop">
      <arg type="b" direction="out" />
    </method>
    <property name="State" type="s" access="read" />
    <property name="ElapsedSeconds" type="t" access="read" />
  </interface>
</node>`;

const RecGoProxy = Gio.DBusProxy.makeProxyWrapper(RecGoIface);

const RecGoIndicator = GObject.registerClass(
class RecGoIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.0, 'RecGo Indicator');

        this._box = new St.BoxLayout({ style_class: 'panel-status-indicators-box' });
        this._icon = new St.Icon({
            icon_name: 'media-record-symbolic',
            style_class: 'system-status-icon',
        });
        this._label = new St.Label({
            text: '',
            y_align: Clutter.ActorAlign.CENTER,
        });

        this._box.add_child(this._icon);
        this._box.add_child(this._label);
        this.add_child(this._box);

        this._buildMenu();
        this._initProxy();
    }

    _buildMenu() {
        this.menu.addAction('Start Recording', () => {
            if (this._proxy) this._proxy.StartRemote();
        });

        this.menu.addAction('Pause / Resume', () => {
            if (this._proxy) this._proxy.PauseRemote();
        });

        this.menu.addAction('Stop Recording', () => {
            if (this._proxy) this._proxy.StopRemote();
        });

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        this.menu.addAction('Open RecGo', () => {
            Gio.AppInfo.create_from_commandline('recgo', 'RecGo', Gio.AppInfoCreateFlags.NONE).launch([], null);
        });
    }

    _initProxy() {
        this._proxy = new RecGoProxy(
            Gio.DBus.session,
            'io.github.antonio.RecGo',
            '/io/github/antonio/RecGo',
            (proxy, error) => {
                if (error) {
                    console.error('Failed to connect to RecGo D-Bus service:', error);
                    return;
                }
                this._updateStatus();
            }
        );
    }

    _updateStatus() {
        if (!this._proxy) return;
        const state = this._proxy.State || 'idle';
        if (state === 'recording') {
            this._icon.style = 'color: #e01b24;'; // Red recording dot
            this._label.text = ' REC';
        } else if (state === 'paused') {
            this._icon.style = 'color: #f6d32d;'; // Yellow paused dot
            this._label.text = ' PAUSED';
        } else {
            this._icon.style = 'color: inherit;';
            this._label.text = '';
        }
    }
});

export default class RecGoExtension extends Extension {
    enable() {
        this._indicator = new RecGoIndicator();
        Main.panel.addToStatusArea(this.uuid, this._indicator);
    }

    disable() {
        if (this._indicator) {
            this._indicator.destroy();
            this._indicator = null;
        }
    }
}
