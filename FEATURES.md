## itr-nvse

### Fixes
- **SlowMotionPhysicsFix** - Stops ragdolls exploding during extreme slow-mo
- **ExplodingPantsFix** - Fixes incendiary ammo damaging the shooter
- **KillActorXPFix** - No XP from console-killing already-dead actors

### Tweaks
- **AltTabMute** - Mutes audio when alt-tabbed
- **ConsoleLogCleaner** - Deletes consoleout.txt on startup
- **MessageBoxQuickClose** - Close message boxes faster
- **AutoGodMode** - Auto-enable tgm on load (dev tool)
- **AutoQuickLoad** - Auto-load quicksave from main menu (dev tool)
- **QuickDrop** - Hotkey to drop equipped weapon
- **Quick180** - Hotkey to turn around instantly

### Script Functions
- **GetRefsSortedByDistance** - Get nearby refs sorted by distance
- **SetOnDialogueTextEventHandler** - Callback when dialogue displays
- **SetOnStealEventHandler** - Callback when stealing occurs
- **SetOnWeaponDropEventHandler** - Callback when actors drop weapons in combat
- **SetOnConsoleOpenEventHandler** - Callback when console opens
- **SetOnConsoleCloseEventHandler** - Callback when console closes
- **SetOnWeaponJamEventHandler** - Callback when weapons jam due to condition
- **Duplicate** - Spawn a copy of any ref at its location, returns new ref
