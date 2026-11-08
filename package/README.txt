EDF6 Spectator
==============

While you're down waiting for a teammate to revive you, your camera follows
your teammates instead of staring at your own body. Two seconds after you go
down it jumps to the first living teammate; when you're revived it comes back
to you on its own.

CONTROLS (while you're down)
----------------------------
  Left click  / RB     next teammate
  Right click / LB     previous teammate

Your own body is the last stop of the cycle. If the teammate you're watching
goes down, the camera moves to the next one; if nobody is left standing it
stays on you and switches as soon as someone is up again.

Only your camera changes: nothing is sent to the other players, so they don't
need the mod.

REQUIREMENTS
------------
  - EDFModLoader: https://github.com/BlueAmulet/EDFModLoader

INSTALL
-------
Copy the Mods folder from this package over the game's, the one next to
EDF6.exe. It ends up like this:

    EARTH DEFENSE FORCE 6\Mods\Plugins\EDF6Spectator.dll
    EARTH DEFENSE FORCE 6\Mods\Spectator\config.ini

The config is optional: without it the mod uses the values above. In it you
can change the delay and the keys.

IF IT DOESN'T WORK
------------------
Everything goes to Spectator.log, next to EDF6.exe:

  - "hooked camera update ..."   the mod found the camera
  - "player N is down"           it noticed you went down
  - "watching player N"          the camera is on a teammate
  - "back to own body: revived"  you were revived

If the first line is missing, the line before it says what didn't match (it
can happen if a game update changed the camera).
