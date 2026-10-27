#!/usr/bin/python3
"""Integration tests: all power-profile and systemd calls use a private bus."""
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import warnings

from gi.repository import Gio, GLib

BINARY = str(Path(sys.argv.pop(1) if len(sys.argv) > 1 else './omapower').resolve())
ADDRESS = os.environ['DBUS_SESSION_BUS_ADDRESS']
PP_NAME = 'net.hadess.PowerProfiles'
PP_PATH = '/net/hadess/PowerProfiles'
COMM = 'oma_evt_test'
SYSTEMD = 'org.freedesktop.systemd1'
MANAGER = '/org/freedesktop/systemd1'
UNIT_PATH = MANAGER + '/unit/omapower_2eservice'
PP_XML = '''<node><interface name="net.hadess.PowerProfiles">
<method name="HoldProfile"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="u" direction="out"/></method>
<method name="ReleaseProfile"><arg type="u" direction="in"/></method>
<signal name="ProfileReleased"><arg type="u"/></signal>
</interface></node>'''
MANAGER_XML = '''<node><interface name="org.freedesktop.systemd1.Manager">
<method name="Subscribe"/><method name="RefUnit"><arg type="s" direction="in"/></method>
<method name="LoadUnit"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
<signal name="Reloading"><arg type="b"/></signal>
<signal name="UnitNew"><arg type="s"/><arg type="o"/></signal>
<signal name="UnitRemoved"><arg type="s"/><arg type="o"/></signal>
</interface></node>'''
UNIT_XML = '''<node><interface name="org.freedesktop.systemd1.Unit">
<property name="LoadState" type="s" access="read"/>
<property name="ActiveState" type="s" access="read"/>
</interface></node>'''
CHILD = '''import ctypes, os, sys, threading
libc = ctypes.CDLL(None)
def rename(name): libc.prctl(15, name.encode(), 0, 0, 0)
rename(sys.argv[1])
print('ready', flush=True)
for line in sys.stdin:
    command, _, value = line.strip().partition(' ')
    if command == 'name': rename(value)
    elif command == 'exec': os.execl('/usr/bin/sleep', 'sleep', '30')
    elif command == 'thread':
        ready = threading.Event()
        def worker():
            rename(value)
            ready.set()
            threading.Event().wait()
        threading.Thread(target=worker, daemon=True).start()
        ready.wait()
    print('ready', flush=True)
'''


def pump(seconds=0):
    end = time.monotonic() + seconds
    while True:
        while GLib.MainContext.default().iteration(False):
            pass
        if time.monotonic() >= end:
            return
        time.sleep(.005)


def until(predicate, timeout=4, message='condition not met'):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        pump()
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError(message)


class Connection:
    def __init__(self, name):
        self.bus = Gio.DBusConnection.new_for_address_sync(ADDRESS,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None, None)
        self.name = name
        self.registrations = []
        self.subscriptions = []

    def export(self, path, xml, method=None, getter=None):
        info = Gio.DBusNodeInfo.new_for_xml(xml)
        self.registrations.append(self.bus.register_object(path, info.interfaces[0], method, getter, None))

    def own(self):
        reply = self.bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus',
            'org.freedesktop.DBus', 'RequestName', GLib.Variant('(su)', (self.name, 0)),
            GLib.VariantType.new('(u)'), Gio.DBusCallFlags.NONE, 1000, None)
        assert reply.unpack()[0] == 1

    def close(self):
        if self.bus is not None:
            for subscription in self.subscriptions:
                self.bus.signal_unsubscribe(subscription)
            for registration in self.registrations:
                self.bus.unregister_object(registration)
            self.bus.close_sync(None)
            self.bus = None
            pump(.02)


class Profiles(Connection):
    def __init__(self):
        super().__init__(PP_NAME)
        self.holds = {}
        self.hold_calls = self.release_calls = 0
        self.release_errors = 0
        self.hold_error = self.delay_hold = self.release_before_reply = False
        self.delayed = None
        self.export(PP_PATH, PP_XML, self.method)
        self.subscriptions.append(self.bus.signal_subscribe('org.freedesktop.DBus',
            'org.freedesktop.DBus', 'NameOwnerChanged', '/org/freedesktop/DBus', None,
            Gio.DBusSignalFlags.NONE, self.owner_changed, None))
        self.own()

    def owner_changed(self, connection, sender, path, interface, member, parameters, data):
        name, old, new = parameters.unpack()
        if name.startswith(':') and not new:
            self.holds = {cookie: owner for cookie, owner in self.holds.items() if owner != name}

    def method(self, connection, sender, path, interface, method, parameters, invocation):
        if method == 'HoldProfile':
            self.hold_calls += 1
            cookie = self.hold_calls
            self.holds[cookie] = sender
            if self.hold_error:
                invocation.return_dbus_error('org.freedesktop.DBus.Error.NoReply', 'ambiguous acquisition')
            elif self.delay_hold:
                self.delayed = invocation, cookie
            elif self.release_before_reply:
                self.manual_release(cookie)
                invocation.return_value(GLib.Variant('(u)', (cookie,)))
            else:
                invocation.return_value(GLib.Variant('(u)', (cookie,)))
        else:
            self.release_calls += 1
            if self.release_errors:
                self.release_errors -= 1
                invocation.return_dbus_error('org.freedesktop.DBus.Error.Failed', 'injected release failure')
            else:
                self.holds.pop(parameters.unpack()[0], None)
                invocation.return_value(None)

    def manual_release(self, cookie=None):
        cookie = cookie or next(iter(self.holds))
        owner = self.holds.pop(cookie)
        self.bus.emit_signal(owner, PP_PATH, PP_NAME, 'ProfileReleased', GLib.Variant('(u)', (cookie,)))
        self.bus.flush_sync(None)


class Systemd(Connection):
    def __init__(self, available=True):
        super().__init__(SYSTEMD)
        self.active = 'inactive'
        self.available = available
        self.calls = []
        self.reads = 0
        self.export(MANAGER, MANAGER_XML, self.method)
        self.export(UNIT_PATH, UNIT_XML, getter=self.getter)
        self.own()

    def method(self, connection, sender, path, interface, method, parameters, invocation):
        self.calls.append(method)
        if method == 'LoadUnit':
            if self.available:
                invocation.return_value(GLib.Variant('(o)', (UNIT_PATH,)))
            else:
                invocation.return_dbus_error(SYSTEMD + '.NoSuchUnit', 'missing service')
        else:
            invocation.return_value(None)

    def getter(self, connection, sender, path, interface, property_name):
        self.reads += 1
        return GLib.Variant('s', self.active if property_name == 'ActiveState' else 'loaded')

    def set_state(self, state):
        self.active = state
        # Invalidation must also work, not just inline property values.
        self.bus.emit_signal(None, UNIT_PATH, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
            GLib.Variant('(sa{sv}as)', (SYSTEMD + '.Unit', {}, ['ActiveState'])))

    def reload(self):
        self.bus.emit_signal(None, MANAGER, SYSTEMD + '.Manager', 'Reloading', GLib.Variant('(b)', (False,)))


class Daemon:
    def __init__(self, commands, poll=False, address=ADDRESS):
        self.temp = tempfile.TemporaryDirectory(prefix='omapower-test-')
        self.base = Path(self.temp.name)
        self.config = self.base / 'config' / 'omapower' / 'config.json'
        self.config.parent.mkdir(parents=True)
        self.runtime = self.base / 'runtime'
        self.runtime.mkdir(mode=0o700)
        self.status = self.runtime / 'omapower.json'
        self.configure(commands)
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address,
            XDG_CONFIG_HOME=str(self.base / 'config'), XDG_RUNTIME_DIR=str(self.runtime),
            G_DEBUG='fatal-warnings', GIO_USE_VFS='local')
        self.process = subprocess.Popen([BINARY] + (['--poll'] if poll else []), env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.log = ''
        until(lambda: self.state() is not None or self.process.poll() is not None)
        assert self.process.poll() is None, self.process.stderr.read()

    def configure(self, commands):
        temporary = self.config.with_suffix('.tmp')
        temporary.write_text(json.dumps({'commands': commands}))
        temporary.replace(self.config)

    def state(self):
        try:
            return json.loads(self.status.read_text())
        except (FileNotFoundError, json.JSONDecodeError):
            return None

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        until(lambda: self.process.poll() is not None, timeout=7)
        _, self.log = self.process.communicate(timeout=1)
        self.temp.cleanup()
        for diagnostic in ('CRITICAL', 'WARNING', 'AddressSanitizer', 'LeakSanitizer', 'runtime error:'):
            assert diagnostic not in self.log, self.log


class Lifecycle(unittest.TestCase):
    def setUp(self):
        self.profiles = Profiles()
        self.daemons = []
        self.children = []

    def tearDown(self):
        for daemon in self.daemons:
            daemon.close()
        for child in self.children:
            if child.poll() is None:
                child.terminate()
            child.communicate(timeout=3)
        self.profiles.close()

    def start(self, commands=(COMM,), poll=False, address=ADDRESS):
        daemon = Daemon(commands, poll, address)
        self.daemons.append(daemon)
        until(lambda: daemon.state()['monitor'] != 'connecting')
        return daemon

    def child(self, name=COMM):
        child = subprocess.Popen(['/usr/bin/python3', '-u', '-c', CHILD, name],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(child)
        self.assertEqual(child.stdout.readline().strip(), 'ready')
        return child

    def command(self, child, command):
        child.stdin.write(command + '\n')
        child.stdin.flush()
        if not command.startswith('exec'):
            self.assertEqual(child.stdout.readline().strip(), 'ready')

    def exit_child(self, child, reap=True):
        child.stdin.close()
        child.stdin = None
        if reap:
            child.wait(timeout=3)

    def held(self, daemon):
        until(lambda: daemon.state()['holding'] and bool(self.profiles.holds))

    def released(self, daemon):
        until(lambda: not daemon.state()['holding'] and not self.profiles.holds)

    def test_idle_status_is_not_rewritten(self):
        daemon = self.start(())
        until(lambda: daemon.state()['available'])
        before = daemon.status.stat()
        pump(1.1)
        after = daemon.status.stat()
        self.assertEqual((before.st_ino, before.st_mtime_ns), (after.st_ino, after.st_mtime_ns))
        self.assertEqual(self.profiles.hold_calls, 0)

    def test_startup_and_overlapping_matches(self):
        first = self.child()
        daemon = self.start()
        self.held(daemon)
        second = self.child()
        pump(.1)
        self.exit_child(first)
        pump(.1)
        self.assertTrue(daemon.state()['holding'])
        self.assertEqual(self.profiles.hold_calls, 1)
        self.exit_child(second)
        self.released(daemon)

    def test_rename_exec_and_thread_names(self):
        daemon = self.start()
        if daemon.state()['monitor'] != 'events':
            self.skipTest('proc connector not available on this kernel/namespace')
        child = self.child('oma_unmatched')
        self.command(child, 'thread ' + COMM)
        pump(.15)
        self.assertEqual(self.profiles.hold_calls, 0)
        self.command(child, 'name ' + COMM)
        self.held(daemon)
        self.command(child, 'name oma_unmatched')
        self.released(daemon)
        self.command(child, 'name ' + COMM)
        self.held(daemon)
        self.command(child, 'exec')
        self.released(daemon)

    def test_zombie_does_not_hold_and_is_not_found_at_startup(self):
        child = self.child()
        daemon = self.start()
        self.held(daemon)
        self.exit_child(child, reap=False)
        until(lambda: os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOWAIT | os.WNOHANG) is not None)
        self.released(daemon)
        second = self.start()
        pump(.1)
        self.assertFalse(second.state()['holding'])
        child.wait(timeout=3)

    def test_exited_leader_with_live_thread_still_holds(self):
        try:
            fd = os.pidfd_open(os.getpid())
        except (AttributeError, OSError):
            self.skipTest('pidfds are unavailable')
        os.close(fd)
        child = subprocess.Popen([str(Path(__file__).with_name('test_process')), '--thread-group'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.children.append(child)
        self.assertEqual(child.stdout.readline().strip(), 'ready')
        until(lambda: Path(f'/proc/{child.pid}/stat').read_text().split(') ', 1)[1][0] == 'Z')
        daemon = self.start()
        self.held(daemon)
        self.exit_child(child)
        self.released(daemon)

    def test_daemon_replacement_reacquires(self):
        self.child()
        daemon = self.start()
        self.held(daemon)
        self.profiles.close()
        self.profiles = Profiles()
        self.held(daemon)
        self.assertEqual(self.profiles.hold_calls, 1)

    def test_daemon_can_appear_after_startup(self):
        self.profiles.close()
        self.child()
        daemon = self.start()
        self.assertFalse(daemon.state()['holding'])
        self.profiles = Profiles()
        self.held(daemon)

    def test_daemon_replacement_during_pending_acquisition(self):
        self.profiles.delay_hold = True
        self.child()
        daemon = self.start()
        until(lambda: self.profiles.delayed is not None)
        self.profiles.close()
        self.profiles = Profiles()
        self.held(daemon)
        self.assertIsNone(daemon.process.poll())

    def test_shutdown_during_pending_acquisition_drops_hold(self):
        self.profiles.delay_hold = True
        self.child()
        daemon = self.start()
        until(lambda: self.profiles.delayed is not None)
        daemon.process.terminate()
        until(lambda: daemon.process.poll() is not None)
        self.assertEqual(daemon.process.returncode, 0)
        until(lambda: not self.profiles.holds)

    def test_new_match_cancels_release_retry(self):
        self.profiles.release_errors = 1
        first = self.child()
        daemon = self.start()
        self.held(daemon)
        self.exit_child(first)
        until(lambda: self.profiles.release_calls == 1)
        second = self.child()
        pump(1.2)
        self.assertTrue(daemon.state()['holding'])
        self.assertEqual(self.profiles.hold_calls, 1)
        self.assertEqual(self.profiles.release_calls, 1)
        self.exit_child(second)
        self.released(daemon)

    def test_release_error_retries_without_reporting_success(self):
        self.profiles.release_errors = 1
        child = self.child()
        daemon = self.start()
        self.held(daemon)
        self.exit_child(child)
        until(lambda: self.profiles.release_calls == 1)
        self.assertTrue(daemon.state()['holding'])
        self.assertTrue(self.profiles.holds)
        self.released(daemon)
        self.assertEqual(self.profiles.release_calls, 2)

    def test_permanent_release_error_disconnects(self):
        self.profiles.release_errors = 20
        child = self.child()
        daemon = self.start()
        self.held(daemon)
        self.exit_child(child)
        until(lambda: daemon.process.poll() is not None, timeout=8)
        self.assertEqual(daemon.process.returncode, 1)
        until(lambda: not self.profiles.holds)
        self.assertEqual(self.profiles.release_calls, 3)

    def test_ambiguous_acquisition_disconnects(self):
        self.profiles.hold_error = True
        self.child()
        daemon = self.start(())
        daemon.configure([COMM])
        until(lambda: daemon.process.poll() is not None)
        self.assertEqual(daemon.process.returncode, 1)
        until(lambda: not self.profiles.holds)
        self.assertEqual(self.profiles.hold_calls, 1)

    def test_exit_during_pending_acquisition(self):
        self.profiles.delay_hold = True
        child = self.child()
        daemon = self.start()
        until(lambda: self.profiles.delayed is not None)
        self.exit_child(child)
        invocation, cookie = self.profiles.delayed
        invocation.return_value(GLib.Variant('(u)', (cookie,)))
        self.released(daemon)
        self.assertEqual(self.profiles.release_calls, 1)

    def test_manual_override_until_last_match_exits(self):
        first = self.child()
        daemon = self.start()
        self.held(daemon)
        self.profiles.manual_release()
        until(lambda: daemon.state()['suppressed'])
        second = self.child()
        self.exit_child(first)
        pump(.15)
        self.assertTrue(daemon.state()['suppressed'])
        self.assertEqual(self.profiles.hold_calls, 1)
        self.exit_child(second)
        until(lambda: not daemon.state()['suppressed'])
        self.child()
        self.held(daemon)
        self.assertEqual(self.profiles.hold_calls, 2)

    def test_release_signal_before_acquisition_reply(self):
        self.profiles.release_before_reply = True
        self.child()
        daemon = self.start()
        until(lambda: daemon.state()['suppressed'])
        self.assertFalse(daemon.state()['holding'])
        self.assertFalse(self.profiles.holds)
        self.assertEqual(self.profiles.hold_calls, 1)

    def test_config_replacement_deletion_and_directory_recreation(self):
        self.child()
        daemon = self.start(())
        daemon.configure([COMM])
        self.held(daemon)
        daemon.configure([])
        self.released(daemon)
        daemon.config.unlink()
        pump(.2)
        daemon.config.parent.rename(daemon.config.parent.with_name('old'))
        pump(.2)
        daemon.config.parent.mkdir()
        daemon.configure([COMM])
        self.held(daemon)
        daemon.configure([])
        self.released(daemon)

    def test_invalid_config_disconnects_and_drops_hold(self):
        self.child()
        daemon = self.start()
        self.held(daemon)
        daemon.config.write_text('invalid')
        until(lambda: daemon.process.poll() is not None)
        self.assertEqual(daemon.process.returncode, 1)
        until(lambda: not self.profiles.holds)

    def test_explicit_polling_fallback(self):
        daemon = self.start(poll=True)
        self.assertEqual(daemon.state()['monitor'], 'polling')
        child = self.child()
        self.held(daemon)
        self.exit_child(child)
        self.released(daemon)

    def test_system_bus_disconnection_exits(self):
        bus = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            address = bus.stdout.readline().strip()
            daemon = self.start((), address=address)
            bus.terminate()
            bus.communicate(timeout=3)
            until(lambda: daemon.process.poll() is not None)
            self.assertEqual(daemon.process.returncode, 1)
        finally:
            if bus.poll() is None:
                bus.terminate()
                bus.communicate(timeout=3)


class ServiceMonitor(unittest.TestCase):
    def setUp(self):
        self.manager = Systemd()
        self.temp = tempfile.TemporaryDirectory(prefix='omapower-widget-test-')
        self.config = Path(self.temp.name) / 'omapower' / 'config.json'
        self.process = subprocess.Popen([BINARY, '--watch-widget'], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=dict(os.environ, G_DEBUG='fatal-warnings', GIO_USE_VFS='local', XDG_CONFIG_HOME=self.temp.name))
        os.set_blocking(self.process.stdout.fileno(), False)
        self.buffer = b''
        self.states = []
        self.configs = []

    def tearDown(self):
        if self.process.poll() is None:
            self.process.terminate()
        until(lambda: self.process.poll() is not None, timeout=7)
        _, log = self.process.communicate(timeout=1)
        self.manager.close()
        self.temp.cleanup()
        for diagnostic in (b'CRITICAL', b'WARNING', b'AddressSanitizer', b'LeakSanitizer', b'runtime error:'):
            self.assertNotIn(diagnostic, log)

    def read_states(self):
        try:
            self.buffer += os.read(self.process.stdout.fileno(), 65536)
        except BlockingIOError:
            pass
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            state = json.loads(line)
            if 'service' in state:
                self.states.append(state['service'])
            if 'configError' in state:
                self.configs.append(state)
        return self.states[-1] if self.states else None

    def test_notifications_no_idle_queries_and_manager_restart(self):
        until(lambda: self.read_states() == 'inactive')
        self.assertEqual(self.manager.calls.count('Subscribe'), 1)
        self.assertEqual(self.manager.calls.count('RefUnit'), 1)
        reads = self.manager.reads
        pump(.3)
        self.assertEqual(reads, self.manager.reads)
        self.manager.set_state('active')
        until(lambda: self.read_states() == 'active')
        self.manager.reload()
        pump(.1)
        self.assertEqual(self.manager.calls.count('RefUnit'), 1)
        self.manager.close()
        until(lambda: self.read_states() == 'unknown')
        self.manager = Systemd()
        until(lambda: self.read_states() == 'inactive')
        self.assertEqual(self.manager.calls.count('Subscribe'), 1)

    def test_configuration_notifications_and_repair(self):
        until(lambda: self.read_states() is not None and bool(self.configs))
        self.assertEqual(self.configs[-1], {'configError': False, 'commands': ['cargo', 'rustc']})
        self.config.write_text('invalid')
        until(lambda: self.read_states() is not None and self.configs[-1]['configError'])
        temporary = self.config.with_suffix('.tmp')
        temporary.write_text('{"commands":["gcc"]}')
        temporary.replace(self.config)
        until(lambda: self.read_states() is not None and self.configs[-1]['commands'] == ['gcc'])
        self.config.parent.rename(self.config.parent.with_name('old'))
        pump(.15)
        self.config.parent.mkdir()
        self.config.write_text('{"commands":["clang"]}')
        until(lambda: self.read_states() is not None and self.configs[-1]['commands'] == ['clang'])
        self.assertFalse(self.configs[-1]['configError'])

    def test_symlink_target_replacement_and_creation(self):
        until(lambda: self.read_states() is not None and bool(self.configs))
        target = Path(self.temp.name) / 'elsewhere' / 'commands.json'
        self.config.symlink_to(target)
        pump(.15)  # dangling target: defaults, but keep observing its ancestor
        target.parent.mkdir()
        target.write_text('{"commands":["gcc"]}')
        until(lambda: self.read_states() is not None and self.configs[-1]['commands'] == ['gcc'])
        temporary = target.with_suffix('.tmp')
        temporary.write_text('{"commands":["clang"]}')
        temporary.replace(target)
        until(lambda: self.read_states() is not None and self.configs[-1]['commands'] == ['clang'])

    def test_missing_unit_recovers_after_reload(self):
        until(lambda: self.read_states() == 'inactive')
        self.manager.close()
        self.manager = Systemd(available=False)
        until(lambda: self.read_states() == 'unknown')
        pump(.05)
        self.manager.available = True
        self.manager.reload()
        until(lambda: self.read_states() == 'inactive')



@unittest.skipUnless(shutil.which('quickshell'), 'quickshell is needed for the headless widget smoke test')
class WidgetSmoke(unittest.TestCase):
    """Use real Quickshell I/O and the real widget, with tiny visual-only stubs."""
    def setUp(self):
        self.manager = Systemd()
        self.temp = tempfile.TemporaryDirectory(prefix='omapower-qml-test-')
        base = Path(self.temp.name)
        self.config_home = base / 'config'
        self.runtime = base / 'runtime'
        self.runtime.mkdir(mode=0o700)
        shell = base / 'shell'
        plugin = shell / 'plugin'
        plugin.mkdir(parents=True)
        project = Path(__file__).resolve().parent.parent
        for name in ('BarWidget.qml', 'Whitelist.js'):
            shutil.copyfile(project / 'plugin' / name, plugin / name)
        stubs = {
            'Ui/qmldir': 'module qs.Ui\nBarWidget 1.0 BarWidget.qml\nWidgetButton 1.0 WidgetButton.qml\n',
            'Ui/BarWidget.qml': 'import QtQuick\nItem { property string moduleName; property var bar: null }',
            'Ui/WidgetButton.qml': '''import QtQuick
Item {
    property var bar
    property string fontFamily
    property bool centerFigures
    property string text
    property string tooltipText
    signal pressed(int buttonCode)
    implicitWidth: 24
    implicitHeight: 24
}''',
            'Commons/qmldir': 'module qs.Commons\nTranslations 1.0 Translations.qml\n',
            'Commons/Translations.qml': '''import QtQml
QtObject { property url directory; function t(key) { return key } }''',
            'plugin/Panel.qml': '''import QtQuick
Item {
    property var bar
    property var anchorItem
    property var hostWidget
    property bool opened: false
    property bool popoutSwitchClosing: false
    function open() { opened = true }
    function close() { opened = false }
    function closeForPopoutSwitch() { opened = false }
    function whitelistEditFinished(success) {}
}''',
            'shell.qml': '''import QtQuick
import Quickshell
import Quickshell.Io
import "./plugin" as Plugin
ShellRoot {
    Plugin.BarWidget { id: widget }
    property string snapshot: JSON.stringify({
        commands: widget.commands, error: widget.whitelistError,
        service: widget.serviceState, holding: widget.holding,
        valid: widget.statusValid, busy: widget.whitelistBusy
    })
    onSnapshotChanged: console.log("OMAPOWER_WIDGET " + snapshot)
    FileView {
        path: Quickshell.env("OMAPOWER_CONTROL")
        watchChanges: true
        onFileChanged: reload()
        onLoaded: {
            const command = JSON.parse(text())
            if (command.add) widget.editWhitelist("--add", command.add)
        }
    }
    Timer { interval: 12000; running: true; onTriggered: Qt.quit() }
}''',
        }
        for name, text in stubs.items():
            path = shell / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        link = self.config_home / 'omarchy' / 'plugins' / 'raw.omapower'
        link.parent.mkdir(parents=True)
        link.symlink_to(project / 'plugin')
        self.control = base / 'control.json'
        self.control.write_text('{}')
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen', DBUS_SYSTEM_BUS_ADDRESS=ADDRESS,
            XDG_CONFIG_HOME=str(self.config_home), XDG_RUNTIME_DIR=str(self.runtime),
            XDG_STATE_HOME=str(base / 'state'), OMAPOWER_CONTROL=str(self.control), GIO_USE_VFS='local')
        env.pop('WAYLAND_DISPLAY', None)
        self.process = subprocess.Popen(['quickshell', '--no-color', '--path', str(shell / 'shell.qml')],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.buffer = b''
        self.output = ''
        self.snapshot = {}

    def tearDown(self):
        if self.process.poll() is None:
            self.process.terminate()
        until(lambda: self.process.poll() is not None, timeout=7)
        self.process.communicate(timeout=1)
        self.manager.close()
        self.temp.cleanup()

    def read_snapshot(self):
        try:
            data = os.read(self.process.stdout.fileno(), 65536)
            self.buffer += data
            self.output += data.decode(errors='replace')
        except BlockingIOError:
            pass
        while b'\n' in self.buffer:
            line, self.buffer = self.buffer.split(b'\n', 1)
            marker = b'OMAPOWER_WIDGET '
            if marker in line:
                self.snapshot = json.loads(line.split(marker, 1)[1])
        return self.snapshot

    def wait(self, predicate):
        try:
            until(lambda: predicate(self.read_snapshot()), timeout=5)
        except AssertionError:
            self.read_snapshot()
            self.fail(self.output)

    def test_widget_updates_without_periodic_queries(self):
        self.wait(lambda state: state.get('service') == 'inactive' and state.get('commands') == ['cargo', 'rustc'])
        config = self.config_home / 'omapower' / 'config.json'
        config.write_text('{"commands":["gcc"]}')
        self.wait(lambda state: state.get('commands') == ['gcc'])
        config.parent.rename(config.parent.with_name('old'))
        pump(.15)
        config.parent.mkdir()
        config.write_text('{"commands":["clang"]}')
        self.wait(lambda state: state.get('commands') == ['clang'])
        config.write_text('invalid')
        self.wait(lambda state: bool(state.get('error')))
        config.write_text('{"commands":["clang"]}')
        self.wait(lambda state: state.get('commands') == ['clang'] and not state.get('error'))
        self.control.write_text('{"add":"gcc"}')
        self.wait(lambda state: state.get('commands') == ['clang', 'gcc'] and not state.get('busy'))
        self.manager.set_state('active')
        self.wait(lambda state: state.get('service') == 'active' and not state.get('valid'))
        status = self.runtime / 'omapower.json'
        status.write_text('{"holding":true,"suppressed":false,"available":true,"monitor":"events"}')
        self.wait(lambda state: state.get('valid') and state.get('holding'))
        self.manager.set_state('inactive')
        self.wait(lambda state: state.get('service') == 'inactive' and not state.get('valid'))
        self.manager.set_state('active')
        self.wait(lambda state: state.get('service') == 'active' and state.get('valid'))
        self.assertNotIn('failed to load', self.output.lower())
        self.assertNotIn('ReferenceError', self.output)

def setUpModule():
    # unittest installs its filters after importing the module. Suppress only
    # PyGObject's known Python 3.14 asyncio-bridge deprecations, not test errors.
    warnings.filterwarnings('ignore', category=DeprecationWarning, module=r'gi\.events')


if __name__ == '__main__':
    unittest.main(verbosity=2)
