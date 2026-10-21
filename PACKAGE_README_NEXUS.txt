Palpedia Enhancer (Nexus Mods package)
======================================
Search, sort and filter the Palpedia, and clear the notification dot in seconds.
Requires Palworld on Steam and UE4SS (RE-UE4SS Okaetsu Experimental-Palworld) from Nexus Mods
(https://www.nexusmods.com/palworld/mods/2237), installed the normal UE4SS way (dwmapi.dll plus
the ue4ss folder next to Palworld-Win64-Shipping.exe). Not the page named just "UE4SS": that is
the 3.x release line and it does not load this mod.

Install
  With Vortex: install and enable like any other mod; Vortex places the files itself.
  By hand:
  1. Install UE4SS (RE-UE4SS Okaetsu Experimental-Palworld) and launch the game once.
  2. Extract this zip into the Palworld install folder (the one containing Palworld.exe and
     Pal\) so that this exists:
       Palworld\Pal\Binaries\Win64\ue4ss\Mods\PalpediaEnhancer\dlls\main.dll
     (the zip already contains the full Pal\Binaries\Win64\ue4ss\Mods\PalpediaEnhancer path
     and an enabled.txt).
  3. Launch Palworld and open the Palpedia.
  To update: replace the folder. To uninstall: delete ue4ss\Mods\PalpediaEnhancer.
  UE4SS-settings.ini must keep HookEngineTick = 1 (the default).

  Subscribed to "UE4SS Experimental (Palworld)" on the Steam Workshop instead? Use the Steam
  Workshop version of this mod, which installs itself:
  https://steamcommunity.com/sharedfiles/filedetails/?id=3788253343

Settings
  Assets\Settings.ini: hotkeys (ToggleUnreadKey, default Ctrl+N; ClearAllKey; MarkAllSeenKey),
  ShowMarkAllSeenButton, RememberState, VerboseLogging, and the state the Palpedia opens in
  (DefaultSort, DefaultSortDescending, DefaultUnreadOnly, DefaultSearchTargets). Every key is
  commented in the file with the values it accepts. This file is replaced when the mod updates.

Bug reports
  Bugs tab on the Nexus Mods page. Include your UE4SS.log
  (Palworld\Pal\Binaries\Win64\ue4ss\UE4SS.log) with VerboseLogging=true, the mod version, the
  Palworld version from the title screen, your game language and your other mods.

License
  License terms are in the Alkeari License Agreement:
  https://gist.github.com/Alkeari/2c6ec0cdf3dafee375b1a00b28ca190a
