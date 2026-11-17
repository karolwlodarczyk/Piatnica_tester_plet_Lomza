# Tester Palet Łomża – skaner wenglor MLBS115

Aplikacja Windows (C++17, Dear ImGui + DirectX 11) do obsługi czujnika 3D wenglor ShapeDrive G4 MLBS115:

- połączenie z czujnikiem, status i temperatura,
- pojedynczy skan na trigger programowy,
- ustawianie najważniejszych parametrów (ekspozycja, moc LED, gain, wzory, okno Z, filtry),
- wymiary palety, przycinanie chmury do jej obrysu,
- podgląd: chmura 3D, mapa głębi, obraz intensywności,
- zapis chmury do pliku TXT (`X Y Z I`).

Parametry czujnika, geometria stanowiska i wszystkie ustalenia z uruchomienia:
**[docs/PARAMETRY.md](docs/PARAMETRY.md)**.

## Wymagania

- Windows 10/11 x64, Visual Studio Build Tools 2022 (MSVC), CMake ≥ 3.20, git.
- SDK wenglor ShapeDrive G4 1.3.0 dla Windows – **nie jest częścią repozytorium**.
  Pobierz `Software_ShapeDrive_G4_SDK_Windows_1.3.0.zip` ze strony produktu
  <https://www.wenglor.com/p/MLBS115> i rozpakuj tak, żeby powstał katalog:

  ```
  vendor/sdk/Software_ShapeDriveG4_SDK_Windows_x86_64_1.3.0/Sensor3D/
      Sensor3d.dll  Sensor3d.lib  sensor3d.h  sensor3ddefines.h
  ```

- Dear ImGui (v1.91.8) pobiera się samo przy konfiguracji CMake.

## Budowanie

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Wynik: `build/Release/WenglorScanner.exe` (obok kopiowana jest `Sensor3d.dll`).

## Uruchamianie

- `WenglorScanner.exe` – okno aplikacji.
- `WenglorScanner.exe --autoscan [ip]` – po starcie od razu łączy się i wykonuje skan.
- `WenglorScanner.exe --selftest <ip> <chmura.txt> <log.txt>` – bez okna: połącz, skanuj, zapisz, wypisz log.

PC musi mieć adres w podsieci czujnika (domyślnie `192.168.100.x/24`).

## Struktura

- `src/Sensor.h`, `src/Sensor.cpp` – obsługa czujnika przez `Sensor3d.dll` w wątku roboczym, skan, analiza, zapis.
- `src/main.cpp` – okno, interfejs, widok 3D.
- `docs/PARAMETRY.md` – parametry i ustalenia.
