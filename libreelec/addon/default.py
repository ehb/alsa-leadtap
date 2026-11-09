# SPDX-License-Identifier: LGPL-2.1-or-later
# The ALSA configuration and wled-audiosync are started by system.d/service.alsa-leadtap.service,
# which LibreELEC's service-addon-wrapper enables when the add-on is installed or enabled. This
# service only (1) tells the user when that failed or something is off (bin/alsa-leadtap leaves
# the reason in STATUS) and (2) restarts the unit when the add-on settings change, so that the
# new settings are written. An audio device that is open keeps its old configuration until it
# is opened again, including its lead; wled-audiosync follows the new lead at once (see README).
#
# Whether settings changed is decided by comparing the effective settings with the ones the unit
# applied (APPLIED, written by "alsa-leadtap start"), not by counting events: a change made in
# the first seconds after Kodi starts is found by the check after the start-up delay, and a
# settings event that changes nothing does not restart the unit.
import os
import subprocess

import xbmc
import xbmcgui

ID = "service.alsa-leadtap"
STATUS = "/run/%s.status" % ID
APPLIED = "/run/%s.applied" % ID
HELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bin", "alsa-leadtap")


class Monitor(xbmc.Monitor):
    def __init__(self):
        xbmc.Monitor.__init__(self)
        self.changed = False

    def onSettingsChanged(self):
        self.changed = True


def read(path):
    try:
        with open(path, encoding="ascii", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return None


def current():
    """The effective settings now, in the form "alsa-leadtap start" stores them; None on error."""
    try:
        out = subprocess.run(["/bin/sh", HELPER, "current"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, timeout=10, check=True)
        return out.stdout.decode("ascii", "replace").strip()
    except (OSError, subprocess.SubprocessError) as e:
        xbmc.log("alsa-leadtap: cannot read the settings: %s" % e, xbmc.LOGWARNING)
        return None


def report():
    reason = read(STATUS)
    if reason:
        xbmc.log("alsa-leadtap: " + reason, xbmc.LOGWARNING)
        xbmcgui.Dialog().notification("alsa-leadtap problem", reason,
                                      xbmcgui.NOTIFICATION_WARNING, 20000)


def restart():
    try:
        subprocess.call(["systemctl", "restart", ID + ".service"], timeout=30)
    except (OSError, subprocess.SubprocessError) as e:
        xbmc.log("alsa-leadtap: restart failed: %s" % e, xbmc.LOGWARNING)
    report()


def main():
    monitor = Monitor()
    # the unit runs before Kodi, but a manual restart may still be under way
    if monitor.waitForAbort(10):
        return
    report()
    # Changes made while we were waiting: nothing was applied for them yet. (No applied
    # settings means the unit is not up; only an explicit change event restarts it then.)
    monitor.changed = False  # cleared first: an event from now on is seen by the loop below
    applied = read(APPLIED)
    if applied is not None and current() not in (None, applied):
        restart()
    while not monitor.waitForAbort(1):
        if not monitor.changed:
            continue
        if monitor.waitForAbort(2):  # let a burst of changes finish
            return
        monitor.changed = False
        applied = read(APPLIED)
        if applied is None or current() != applied:
            restart()


main()
