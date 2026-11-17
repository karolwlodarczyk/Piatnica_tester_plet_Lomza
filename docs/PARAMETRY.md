# Wenglor MLBS115 – parametry i ustalenia (Tester Palet Łomża)

Stan na 2026-11-17. Wszystko poniżej zostało odczytane z czujnika albo zmierzone skanami,
chyba że zaznaczono inaczej. Dokument ma wystarczyć do napisania innego oprogramowania
(np. systemu wizyjnego) bez powtarzania tych prób.

## 1. Urządzenie

| | |
|---|---|
| Model | wenglor ShapeDrive G4 MLBS115 |
| Firmware / SDK | 1.3.2 / 1.3.0 |
| Matryca | 2448 × 2048 px (5 013 504 punktów na skan) |
| Obszar pomiarowy | X 1300 mm, Y 1000 mm, Z 1750–2750 mm od czujnika |
| Temperatura pracy (odczyt) | ok. 45 °C |

Obszar pomiarowy jest prostopadłościanem: punkty są obcinane na X ±650 i Y ±500 mm
niezależnie od Z. Poniżej 1750 mm czujnik nie zwraca pomiarów.

## 2. Sieć

- IP czujnika: `192.168.100.1/24` (fabryczne), brak DHCP.
- PC musi mieć adres w tej podsieci, np. `192.168.100.10/24`
  (`New-NetIPAddress -InterfaceAlias "Ethernet 2" -IPAddress 192.168.100.10 -PrefixLength 24`, jako administrator).
- Otwarte porty: 80 i 8042 (panel WWW „Wenglor Administration"), 8080, 22, 1534, 32001 i 32003 (SDK).
- Czujnik nie odpowiedział na discovery GigE Vision z innej podsieci. Wykryty pasywnie:
  co kilka sekund rozgłasza pakiet UDP 1534 (Xilinx TCF Agent) ze swoim IP.

## 3. Komunikacja

Wenglor nie dokumentuje surowego protokołu TCP. Jedyna udokumentowana droga to biblioteka
`Sensor3d.dll` (x64, wywołania C) z komendami ASCII. Wariant GigE Vision istnieje, ale nie był testowany.

Funkcje: `Sensor3D_Connect(ip, timeout_ms)`, `Sensor3D_Disconnect`, `Sensor3D_GetSensorStatus`,
`Sensor3D_ReadData(h, "GetXxx", buf, size, 0)`, `Sensor3D_WriteData(h, "SetXxx=wartość\r")`,
`Sensor3D_GetPointCloud`.

Uwagi praktyczne:

- Komendy zapisu kończyć znakiem `\r`.
- Bity statusu: `0x1` połączony, `0x4` trwa akwizycja, `0x8` przegrzanie.
- W trakcie akwizycji czujnik odrzuca większość komend (`-201 BUSY`); dozwolone są tylko
  `SetGain`, `SetExposureTime`, `SetLEDPower`, `SetTriggerSoftware`, `SetAcquisitionStop`.
- Temperatura: `GetOperatingTemperature` (tekst, np. `44.9`).
- Tylko jedna aplikacja naraz powinna trzymać połączenie.

### Sekwencja pojedynczego skanu

```
SetSensorMode=4          (chmura punktów)
SetTriggerSource=1       (trigger programowy)
SetAcquisitionStart
(odczekać ok. 100 ms)
SetTriggerSoftware
Sensor3D_GetPointCloud(..., timeout 20 s)
SetAcquisitionStop
```

Czas skanu: 3–4 s przy 28 wzorach i ekspozycji 30 ms.

### Format danych

- Bufor: `POINT_CLOUD { POINT3D* point; uint16_t* intensity; uint16_t* confidence; }`,
  `POINT3D` = 3 × `double` (mm). Rozmiar bufora podawany do SDK: `(24 + 2) * liczba_pikseli`,
  `confidence = nullptr`.
- Chmura jest uporządkowana: indeks `i = y * 2448 + x`, zawsze 5 013 504 punktów.
- **Piksel bez pomiaru ma Z = -1** (często też X = Y = 0, ale nie zawsze). Filtr: `z > 0`.
- Układ współrzędnych: początek w czujniku, Z rośnie w dół (od czujnika), X wzdłuż dłuższego boku pola.

## 4. Parametry czujnika

| Parametr (komenda) | Wartość robocza | Zakres | Uwagi |
|---|---|---|---|
| `LEDPower` | **60 %** | 0–100 | fabrycznie było 10 % – za mało, patrz pkt 6 |
| `ExposureTime` | 30000 µs | 0–80527 | bez zmian |
| `ExposureTimeLimit` | 30001 µs | 12000–80527 | bez zmian; zmiana trwa do 5 s |
| `Gain` | 0 | 0–100 | nie podnosić: przy 50 prześwietla 44 % obrazu |
| `LEDPattern` | 28 | 16 / 28 | 28 = dokładniej, 16 = szybciej |
| `ContrastComparisonFilterMinPhase` | **0.05** | 0–1 | fabrycznie 0.10 |
| `Zmin` / `Zmax` | **1850 / 2100 mm** | 1750–2750 | okno na paletę; **nie jest pamiętane po rozłączeniu** |
| `HDRMode` | 0 | 0 / 1 | wpływa tylko na obraz intensywności |
| `SubSampling` | 0 | 0 / 1 | |
| `EnableExtendedMeasuringRange` | 0 | 0 / 1 | nie pomaga, dokładność poza zakresem niegwarantowana |
| `SDKSOutlierFilter` | 0 | 0 / 1 | filtr po stronie PC, opcjonalny |
| `SDKMedianFilter` | 0 | 0 / 1 | filtr po stronie PC, opcjonalny |

Trwałość ustawień:

- `LEDPower` przetrwał rozłączenie i ponowne połączenie.
- `Zmin`/`Zmax` wracają do 1750/2750 po każdym połączeniu – trzeba je ustawiać po `Connect`.
  Kolejność: czujnik wymaga `Zmin < Zmax` w każdej chwili.
- Nie sprawdzono, co przetrwa wyłączenie zasilania. Bezpiecznie: po każdym połączeniu
  wysłać komplet parametrów z tabeli.
- `LEDActivate` odczytywał się niespójnie (raz 0, raz 1) i nie miał wpływu na liczbę punktów.
  Nie polegać na nim.

## 5. Geometria stanowiska

| | |
|---|---|
| Paleta | 1200 × 800 mm, wysokość max 150 mm |
| Orientacja | dłuższy bok wzdłuż osi X czujnika |
| Powierzchnia rolek | Z ≈ 2050 mm od czujnika |
| Wierzch pokładu palety | Z ≈ 1885–1905 mm |
| Zapas pola widzenia | 50 mm na stronę w X, 100 mm w Y |

Zmierzone na skanie palety:

- Pokład zajmuje X ≈ −537 … +650 mm: paleta jest przesunięta ok. 50 mm w stronę +X
  i jej krawędź leży na granicy pola. Do poprawy mechanicznie albo trzeba to uwzględnić w analizie.
- Pokład opada ok. 16 mm na 800 mm wzdłuż Y (ok. 1,2°), wzdłuż X jest płaski (1,4 mm na 1200 mm).
  Nie rozstrzygnięto, czy to pochylenie czujnika, czy palety – przed pomiarami wysokości
  wykonać skan referencyjny pustych rolek lub płyty wzorcowej.
- Odchylenie punktów pokładu od dopasowanej płaszczyzny: ok. 1,4–1,7 mm RMS (zawiera nierówności desek).

Minimalna odległość montażu: wierzch palety ≥ 1750 mm od czujnika, czyli rolki ≥ 1900 mm
przy palecie 150 mm; rolki ≤ 2750 mm.

## 6. Wyniki optymalizacji obrazu

Pokrycie = odsetek pikseli z pomiarem na wysokości pokładu (maksimum ok. 80–90 %, reszta to
szczeliny między deskami). Narożniki: okna 380 × 380 px.

| Moc LED | Paleta | Lewy górny | Prawy górny | Lewy dolny | Prawy dolny |
|---|---|---|---|---|---|
| 10 % | 70,9 % | 71,1 % | 50,3 % | 61,2 % | 70,5 % |
| 30 % | 79,9 % | 85,3 % | 83,7 % | 66,3 % | 88,7 % |
| 60 % | 80,3 % | 85,9 % | 84,8 % | 66,5 % | 89,1 % |
| 100 % | 80,4 % | 86,2 % | 84,9 % | 66,6 % | 89,1 % |

- Przyczyną słabego narożnika było niedoświetlenie. Powyżej 30 % zysk jest znikomy;
  60 % zostawia zapas na ciemne lub mokre drewno przy 0,5 % prześwietlonych pikseli.
- Filtr kontrastu 0.10 → 0.05: +0,4 pkt proc.; 0.02: +1,7 pkt proc., ale większy szum.
- Błyszczące stalowe rolki mierzą się słabo i z odblaskami – nie traktować ich jako pewnej powierzchni odniesienia.

## 7. Plik chmury (TXT)

Jedna linia na punkt: `X Y Z I`, wartości w mm z 3 miejscami po przecinku, separator spacja,
kropka dziesiętna, bez nagłówka. `I` = intensywność 0–65535. Zapisywane są tylko punkty
z pomiarem, opcjonalnie przycięte do obrysu palety (|X| ≤ 650, |Y| ≤ 450 mm). Kolejność
punktów w pliku to kolejność pikseli, ale po odfiltrowaniu pustych siatka nie jest już zachowana.
