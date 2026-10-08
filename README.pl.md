# Metin2 WebClient

[![tests](https://github.com/nairoad/Metin2-WebClient/actions/workflows/tests.yml/badge.svg)](https://github.com/nairoad/Metin2-WebClient/actions/workflows/tests.yml)

**English version: [README.md](README.md).**

Graj na swoim serwerze Metin2 w przeglądarce. Ten zestaw bierze **Twoje**
źródła klienta i **Twoje** pliki gry i buduje z nich wersję klienta gry dla
przeglądarki (`client.html` + `client.wasm`) oraz mały most, którego
przeglądarka potrzebuje, żeby rozmawiać z serwerem gry. Gracze otwierają
stronę i grają - bez instalowania czegokolwiek.

Działa ze źródłami klasycznego klienta TMP4 („mainline”) i jest pomyślany
tak, żeby dało się go dostosować do innych forków. Co dziś działa:
logowanie, wybór postaci, świat, walka, konie, umiejętności, sklepy, misje,
czat, znajomi, opcje.

> To repozytorium zawiera tylko zestaw (nasz kod i narzędzia). **Nie** ma
> w nim plików gry, źródeł klienta ani żadnego cudzego SDK - przynosisz
> swoje.

---

## Czego potrzebujesz

**Komputera z Windows 10/11** (budowanie działa na Windows; innych systemów
nie testowano), około **6 GB wolnego miejsca** oraz:

| przynosisz | co to jest |
|---|---|
| **źródła** klienta | folder z kodem C++ klienta, ten z `source/` (GameLib, EterLib, UserInterface...) i `extern/` |
| zainstalowanego **klienta gry** | folder z `pack/` (pliki `.eix`/`.epk`) - ten sam klient, którego używają gracze |
| **adres serwera** | adres, z którym łączą się gracze |

Zainstaluj raz te narzędzia:

| narzędzie | jak | po co |
|---|---|---|
| [Python 3.13](https://www.python.org/downloads/) | instalator, zaznacz „Add to PATH” | prowadzi całą budowę (3.11/3.12 też) |
| [Python 3.10](https://www.python.org/downloads/release/python-31011/) (albo dowolny 3.8-3.12) | instalator, **nie** trzeba dodawać do PATH | przepisuje stare skrypty gry z Pythona 2 (potrzebuje `lib2to3`, usuniętego w 3.13) |
| [Git for Windows](https://git-scm.com/download/win) | instalator | do pobrania tego projektu i Emscripten SDK |
| [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) **6.0.8** | `git clone https://github.com/emscripten-core/emsdk.git C:\emsdk`, potem w `C:\emsdk`: `emsdk install 6.0.8` i `emsdk activate 6.0.8` | kompilator C++ -> WebAssembly. Użyj 6.0.8: budowa pobiera gotowy silnik Pythona zrobiony dokładnie tą wersją. W innym miejscu niż `C:\emsdk`? ustaw `[tools] emsdk` |
| Pillow | `pip install pillow` | przerabia kursory myszy |
| [Node.js 20+](https://nodejs.org/) | instalator (daje `npm`) | uruchamia most między przeglądarką a serwerem |

Opcjonalnie, dla lepszego wyniku:

| narzędzie | daje |
|---|---|
| [Visual Studio Build Tools 2022](https://visualstudio.microsoft.com/downloads/) z „Desktop development with C++” (x86) | drzewa na mapach oraz ~1% modeli, których inaczej by brakowało |
| Microsoft Edge | automatyczne sprawdzenia jakości (`python webclient.py check`) |

## Szybki start

**1. Pobierz zestaw**

```
git clone https://github.com/nairoad/Metin2-WebClient.git
cd Metin2-WebClient
```

**2. Wskaż, gdzie są Twoje pliki.** Skopiuj `webclient.example.toml` do
`webclient.toml` i uzupełnij trzy linie (w ścieżkach używaj `/`):

```toml
[inputs]
source = "C:/sciezka/do/zrodel-klienta"
client = "C:/sciezka/do/klienta-gry"

[server]
address = "adres.twojego.serwera"
```

Jeśli Python 3.10 nie leży w domyślnym miejscu, ustaw też `python2to3`
w `[tools]` - każdy klucz jest opisany w `webclient.example.toml`.

**3. Sprawdź, czy wszystko jest na miejscu**

```
python webclient.py doctor
```

Każdy brak to jedna linia z instrukcją naprawy. Powtarzaj, aż pojawi się
`ready to build`.

**4. Zbuduj** (pierwsza budowa trwa około 45 minut - ponad połowa to
jednorazowe sprawdzenie wszystkich modeli; kolejne kilka minut)

```
python webclient.py build
```

Zatrzymuje się na pierwszym problemie, mówi jednym zdaniem, co zrobić,
i podaje, jak wznowić od tego kroku.

**5. Graj**

```
cd bridge
npm ci
cd ..
python webclient.py serve
```

(`npm ci` tylko za pierwszym razem - instaluje most.) Otwórz
**http://127.0.0.1:8731/client.html** i zaloguj się. Ctrl+C zatrzymuje.

## Umieść na swojej stronie

`python webclient.py package` składa wszystko do wgrania w `dist/site/`
(około 2 GB, głównie dane gry). Na serwerze WWW uruchamiasz też most
(`python tools/bridge_config.py --site https://twoja.strona` robi jego
ustawienia) i każesz nginx przekazywać do niego `/to/`, żeby przeglądarki
dosięgły gry przez HTTPS. Czym jest most i każdy krok, z przykładem nginx:
[docs/WDROZENIE.md](docs/WDROZENIE.md#3-most---jak-gra-dociera-do-twojego-serwera).

## Gdy coś nie działa

| co widzisz | co zrobić |
|---|---|
| `doctor` pokazuje `FAIL` | zrób to, co podaje linia pod spodem; uruchom `doctor` ponownie |
| budowa staje na `stage` z `DID NOT HIT` | Twoje źródła różnią się od TMP4 w miejscu, które zestaw łata - patrz [docs/BUILDING.md](docs/BUILDING.md#4-when-a-step-fails) |
| budowa staje z `error:` w jakimś pliku | fragment Twoich źródeł, którego zestaw jeszcze nie obsługuje - linia podaje plik |
| krok `gr2` mówi, że pomocnika „cannot be started” | antywirus usunął świeżo zbudowane narzędzie - dopuść folder budowy albo pomiń: `python webclient.py build --skip gr2` |
| gra czeka na „łączenie z serwerem” | most nie działa - użyj `python webclient.py serve` (albo `python webclient.py bridge`) |
| wylogowuje po wyborze postaci | serwer odesłał grę na port, którego most nie dopuszcza - dopisz go: `[bridge] targets = ["adres:port"]` w `webclient.toml` |
| napisy wyglądają źle, brak drzew | przy budowie brakowało narzędzi opcjonalnych - patrz `doctor` (`windows-gdi`, `msvc`, `speedtree`) |

Więcej: [docs/BUILDING.md](docs/BUILDING.md) (każdy krok i każde narzędzie, po angielsku).

## Opcje w adresie strony

Dopisz do adresu, np. `client.html?scale=1.5`:

| opcja | znaczenie |
|---|---|
| `scale=1..3` | rozmiar interfejsu (1, 1.25, 1.5, 1.75, 2, 2.5, 3); zapamiętywany |
| `return=<url>` | dokąd prowadzi „Wyjdź z gry”, gdy karta nie może się sama zamknąć (tylko http/https) |
| `lang=pl` / `deflang=pl` | język gry / język domyślny |

Na Twojej prawdziwej stronie działają tylko te opcje dla graczy (oraz
`cursor`, `fps`, `max`, `corpusMB`, `noprefetch`). Pozostałe - np. `bridge=`
czy `corpus=` - działają tylko na stronie lokalnej (`127.0.0.1`), więc nikt
nie wyśle Twoim graczom linku do Twojej własnej strony, który rozmawia
z innym serwerem. Most pod osobnym adresem ustawia się przy budowie:
`[server] bridge` w `webclient.toml`.

Pełna lista (także przełączniki diagnostyczne): `m2w.options.table`
w konsoli przeglądarki albo [docs/REFERENCE.md](docs/REFERENCE.md).

---

## Dla programistów

Jak działa port: oryginalny silnik C++ jest kompilowany Emscriptenem;
warstwa zgodności zastępuje to, czego silnik oczekiwał od Windows -
Direct3D 8 -> WebGL 2, Win32 -> API przeglądarki, Miles Sound -> Web Audio,
Granny 3D -> własny czytnik `.gr2`, tekst GDI -> czcionki wypalone
prawdziwym GDI, SpeedTree RT -> drzewa wypalone offline. Dane gry spływają
z serwera w kawałkach po 4 MB („korpus”) i przeglądarka je zapamiętuje.

```
compat/          warstwa zgodności (C++/JS) - patrz docs/REFERENCE.md
  tree/          nasze nagłówki zastępujące kilka nagłówków silnika
  tests/         testy jednostkowe
site/            strona, Service Worker, pasek pobierania
bridge/          most WebSocket -> TCP (Node.js)
tools/           narzędzia budowy, pakowania i wypalania; workspace.py czyta webclient.toml
tools/gates/     bramki jakości (eksporty, shadery, sonda modelu, ekran logowania, testy, opisy)
webclient.py     jedno polecenie, które prowadzi wszystko
docs/            BUILDING, DEPLOYMENT (EN) / WDROZENIE (PL), REFERENCE (generowany)
```

- Każda funkcja ma opis `///` (pilnuje tego `tools/gates/check_docs.py`);
  `docs/REFERENCE.md` wymienia każdy plik, funkcję i opcję adresu.
- Tam, gdzie źródła silnika musiały się zmienić, `tools/stage_port.py`
  łata kopię podczas składania drzewa budowy; każda łata mówi, co naprawia
  i dlaczego.
- Komentarze mówią DLACZEGO: co zmierzono, co próbowano i co nie zadziałało.
- Po zmianie: `python webclient.py check`. Różnica to albo błąd, albo
  zamierzona zmiana (wtedy `python webclient.py check --record`).

## Bezpieczeństwo

Znalazłeś lukę? Zgłoś ją prywatnie - zob. [SECURITY.md](SECURITY.md).

## Licencja

GPL-2.0-or-later dla wszystkiego w tym repozytorium (wymusza to LZO, które
klient linkuje). Copyright (C) 2026 nairoad - zob. [LICENSE](LICENSE) i
[NOTICE](NOTICE). Źródła silnika, dane gry i SDK używane przez wypalarki
offline **nie** są częścią tego repozytorium i nie obejmuje ich ta
licencja; ich status jest taki, jak każdego serwera prywatnego Metin2.
Wypalarka SpeedTree tylko woła SDK na Twoim komputerze - nic z niego nie
trafia do `client.wasm`.

## Podziękowania

- Ymir Entertainment - silnik Metin2 (TMP4).
- Powstało runda po rundzie z Claude (Anthropic).

Jeśli ten projekt Ci się przydał - uruchamiasz go na swoim serwerze,
budujesz na nim coś własnego albo po prostu czegoś się z niego nauczyłeś -
byłoby mi miło, gdybyś wspomniał, skąd pochodzi. Krótkie „thanks to nairoad”
z linkiem tutaj w zupełności wystarczy. To nie jest warunek licencji, tylko
sposób, żebym wiedział, że ta praca do kogoś trafiła. Dzięki!
