# Wdrożenie — hostowanie klienta na własnej stronie

English version: [DEPLOYMENT.md](DEPLOYMENT.md).

Klient to statyczna strona plus most WebSocket→TCP. Na serwerze nie działa
nic poza tym mostem i serwerem plików.

## 1. Pliki do wgrania — `python webclient.py package` składa `dist/site/` dokładnie z tym zestawem

| plik / katalog | rozmiar | uwagi |
|---|---|---|
| `client.html` | 3 kB | nasza strona (`site/client.html`) |
| `client.js`, `client.wasm` | 0,5 + 11 MB | silnik; adresy niosą `?v=<mtime>`, więc po przebudowie nigdy nie idzie stara wersja |
| `client.data` | ~40 MB | pakiet startowy: skrypty Pythona, UI, czcionki, kursory |
| `preload.js`, `sw.js` | małe | pasek pobierania i Service Worker |
| `corpus/` | ~2 GB | `manifest.bin`, `order.txt` i ~500 kawałków `<hash>.bin` (adresowane treścią: SHA-256 zawartości obcięte do 16 bajtów) |

**Nie** wgrywać `private/`, `stage/`, `obj/`, `lib/`, `data/`, `bake/`.

## 2. Serwer HTTP

Wystarczy zwykły hosting statyczny (nginx, Apache, Caddy). Zalecane nagłówki
(cała strona nginx z nimi: część 3, krok 5):

```
corpus/*.bin          Cache-Control: public, max-age=31536000, immutable
corpus/manifest.bin   Cache-Control: no-cache
corpus/order.txt  Cache-Control: no-cache
client.html                   Cache-Control: no-cache
client.js / .wasm / .data     Cache-Control: public, max-age=31536000  (wersjonowane przez ?v=)
*.wasm                        Content-Type: application/wasm
```

Włącz gzip/brotli dla `.js`, `.data` i `.html` (kawałki to już skompresowane
dane — `.bin` nie kompresować drugi raz).

HTTPS jest wymagane dla Service Workera i dla `wss://` (przeglądarka
odmawia zwykłego `ws://` ze strony https, poza localhost).

Tylko w sieci domowej (np. `http://192.168.1.10/metin2/client.html`) działa
też zwykłe HTTP: klient używa wtedy `ws://` i działa bez Service Workera
i Cache Storage (przeglądarki pozwalają na nie tylko po HTTPS), więc przy
każdej wizycie pobiera dane gry z Twojego serwera od nowa - w sieci lokalnej
to szybkie. Dla graczy z internetu używaj HTTPS.

## 3. Most - jak gra dociera do Twojego serwera

### Co robi

Gra rozmawia ze swoim serwerem zwykłym TCP (port logowania, np. 11000,
i porty kanałów, np. 13000). **Przeglądarka nie potrafi otwierać połączeń
TCP** - umie tylko WebSockety. Most (`bridge/`, mały program w Node.js)
stoi pośrodku: klient otwiera do niego WebSocket, a most otwiera połączenie
TCP z serwerem gry i przekazuje bajty w obie strony, bez zmian.

```
przeglądarka gracza
   |
   |  https://twoja.strona/metin2/client.html        -> nginx podaje pliki gry
   |  wss://twoja.strona/to/<adres gry>:<port>       -> nginx przekazuje do mostu
   v
nginx (twoja.strona, port 443)
   |  location /to/  ->  http://127.0.0.1:11496
   v
most (127.0.0.1:11496, na tej samej maszynie)
   |  zwykłe TCP, tylko do adresów z TWOJEJ listy serwerów
   v
serwer gry (logowanie 11000, kanały 13000...)
```

Adres mostu klient składa sam: strona otwarta z `https://twoja.strona` łączy
się z `wss://twoja.strona/to/...` - ta sama strona i port co strona gry,
ścieżka `/to/`. Na serwerze WWW wystarczy więc, żeby nginx przekazywał `/to/`
do mostu. (Lokalnie `python webclient.py serve` uruchamia most za Ciebie
pod `127.0.0.1:11496`, gdzie klient go szuka, gdy strona jest na localhost.)

Most **nie jest otwartym przekaźnikiem**: łączy się tylko z adresami z listy
serwerów Twojej gry (odczytanej ze zbudowanych danych klienta) oraz z pasmem
`[bridge] port_span` portów powyżej każdego - rdzeniami gry, na które serwer
odsyła klienta po wyborze postaci - plus `[bridge] targets` z
`webclient.toml`; a połączenia przyjmuje tylko od strony Twojej gry.
**Na tych portach hosta serwera gry nie może słuchać nic poza grą**
(np. 11000-11099, 13000-13129 przy domyślnym paśmie): most wpuściłby tam
internet. Zmniejsz `[bridge] port_span` do tego, czego używają Twoje rdzenie.

### Krok po kroku (serwer z Linuksem i nginx)

**1. Zrób konfigurację mostu** - na komputerze, na którym budowałeś
klienta, z adresem swojej strony (schemat i host, bez ścieżki):

```
python tools/bridge_config.py --site https://twoja.strona
```

Jeśli gracze otwierają grę pod kilkoma adresami (np. domena, a w domu adres
serwera WWW w sieci lokalnej), powtórz `--site` dla każdego:
`--site https://twoja.strona --site http://192.168.1.10`. Strona spoza listy
dostaje od mostu `401` i gra nie może się połączyć.

Zapisuje `build/port/private/bridge-site.json`. W pliku jest adres Twojego
serwera gry - trzymaj go prywatnie, nie wrzucaj do repozytorium.

**2. Skopiuj most na serwer** - folder `bridge/` z tego repozytorium (bez
`node_modules`) i `bridge-site.json`, np. do `/opt/metin2-bridge/`.

**3. Zainstaluj i spróbuj** - na serwerze, z Node.js 20 lub nowszym:

```
cd /opt/metin2-bridge
npm ci
node src/index.js bridge-site.json
```

Wypisuje jedną linię na zdarzenie, zaczynając od `{"event":"listening",...}`.
Zatrzymujesz Ctrl+C.

**4. Niech działa stale** - jako usługa systemd,
`/etc/systemd/system/metin2-bridge.service`:

```
[Unit]
Description=Metin2 WebClient bridge (WebSocket -> TCP)
After=network.target

[Service]
WorkingDirectory=/opt/metin2-bridge
ExecStart=/usr/bin/node src/index.js bridge-site.json
Restart=always
RestartSec=3
User=www-data
# hardening: the bridge only reads its folder and opens network connections
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX

[Install]
WantedBy=multi-user.target
```

```
sudo systemctl enable --now metin2-bridge
journalctl -u metin2-bridge -f        # jego dziennik
```

**5. nginx** - pliki gry (ZAWARTOŚĆ `dist/site/` z `python webclient.py
package`: `client.html`, `client.js`, `client.wasm`, `client.data`,
`preload.js`, `sw.js`, `corpus/`) wgraj do `/var/www/metin2/`, a potem daj
nginx tę stronę. W Debianie/Ubuntu zapisz ją jako
`/etc/nginx/sites-available/metin2` i włącz przez
`sudo ln -s /etc/nginx/sites-available/metin2 /etc/nginx/sites-enabled/`;
w innych systemach zapisz jako `/etc/nginx/conf.d/metin2.conf`.

```
server {
    listen 443 ssl;
    server_name twoja.strona;
    ssl_certificate     /etc/letsencrypt/live/twoja.strona/fullchain.pem;   # np. z certbot
    ssl_certificate_key /etc/letsencrypt/live/twoja.strona/privkey.pem;

    # pliki gry: /metin2/... -> /var/www/metin2/...
    # "no-cache" = przeglądarka za każdym razem pyta, czy plik się zmienił,
    # i dostaje krótkie "bez zmian", gdy nie - więc aktualizacja od razu
    # dociera do graczy (client.html, sw.js, preload.js, corpus/manifest.bin).
    location /metin2/ {
        root /var/www;
        add_header Cache-Control "no-cache";
        add_header X-Content-Type-Options "nosniff" always;
        add_header Content-Security-Policy "frame-ancestors 'self'" always;
    }

    # silnik: client.html prosi o client.js?v=<czas budowy>, więc nowa
    # budowa to nowy adres - przeglądarka może trzymać każdy rok
    location ~ ^/metin2/client\.(js|wasm|data)$ {
        root /var/www;
        add_header Cache-Control "public, max-age=31536000";
        add_header X-Content-Type-Options "nosniff" always;
        add_header Content-Security-Policy "frame-ancestors 'self'" always;
    }

    # kawałki danych mają w nazwie skrót swojej treści, więc kawałek nigdy
    # się nie zmienia - przeglądarka może go trzymać rok
    location ~ ^/metin2/corpus/[0-9a-f]+\.bin$ {
        root /var/www;
        add_header Cache-Control "public, max-age=31536000, immutable";
        add_header X-Content-Type-Options "nosniff" always;
        add_header Content-Security-Policy "frame-ancestors 'self'" always;
    }

    # most: każdy WebSocket /to/... idzie do niego
    location /to/ {
        proxy_pass         http://127.0.0.1:11496;
        proxy_http_version 1.1;
        proxy_set_header   Upgrade $http_upgrade;
        proxy_set_header   Connection "upgrade";
        proxy_set_header   Host $host;
        proxy_set_header   X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_read_timeout 3600s;
        proxy_send_timeout 3600s;
    }
}
```

Potem `sudo nginx -t && sudo systemctl reload nginx`.

Po co dodatkowe linie: `nosniff` nie pozwala przeglądarkom zgadywać typu
plików, a `frame-ancestors 'self'` nie pozwala innym stronom pokazywać Twojej
gry w ramce (kliknięcia wyłudzane przez niewidoczną ramkę). nginx pomija
nagłówki z bloku `server` w każdym `location`, który ustawia własne, dlatego
są powtórzone w każdym. Gdy HTTPS działa na stałe, rozważ też
`Strict-Transport-Security` (certbot potrafi go dodać). `bridge-site.json`
niech czyta tylko usługa: `sudo chown root:www-data bridge-site.json &&
sudo chmod 640 bridge-site.json` - jest w nim adres Twojego serwera gry.

**Opcjonalnie - limit połączeń na adres gracza.** Most zapisuje w dzienniku
adres gracza (z `X-Forwarded-For`, który ustawia nginx powyżej) i potrafi
odmówić więcej niż N sesji z jednego adresu: `[bridge] max_per_address = 20`
w `webclient.toml`, potem konfiguracja ponownie (krok 1). Tylko z PRAWDZIWYM
adresem gracza: za Cloudflare nginx widzi adresy Cloudflare, więc najpierw
dodaj do bloku `server` `real_ip_header CF-Connecting-IP;` i linię
`set_real_ip_from <zakres>;` dla każdego zakresu z
https://www.cloudflare.com/ips/ - inaczej wszyscy gracze dzielą kilka
adresów i limit by ich zablokował.

- **Tylko w sieci domowej, bez certyfikatu:** pierwsze cztery linie zamień
  na `listen 80;` i `server_name 192.168.1.10;` (adres serwera WWW),
  a grę otwieraj pod `http://192.168.1.10/metin2/client.html`.
- **Na serwerze jest już strona na tym samym porcie i nazwie** (np. strona
  `default` na porcie 80): nie dodawaj drugiego bloku `server` - skopiuj
  cztery bloki `location` do istniejącego albo wyłącz tamtą stronę
  (`sudo rm /etc/nginx/sites-enabled/default` usuwa tylko skrót).
- **Pozostałości wcześniejszej konfiguracji:** inne bloki `location`
  z wyrażeniem regularnym dla `.bin` / `.wasm` (np.
  `location ~* \.(bin|wasm)$`) wygrywają z `location /metin2/` - usuń je
  (dlaczego - krok 6).
- **Za Cloudflare:** WebSockety zostaw włączone (ustawienia Network,
  domyślnie włączone) i tryb SSL „Full". Most co 30 s pinguje każdą
  przeglądarkę, więc Cloudflare nie zrywa cichej gry.
- Jeśli `client.wasm` nie przychodzi z `Content-Type: application/wasm`
  (stary nginx), dopisz linię `application/wasm wasm;` do listy `types`
  w `/etc/nginx/mime.types` (nie blok `types` w stronie - ten zastąpiłby
  całą listę i zepsuł pozostałe pliki).

**6. Sprawdź**

- `curl -i https://twoja.strona/to/ping` odpowiada `426 Upgrade Required` -
  nginx dociera do mostu (zwykłe żądanie to nie WebSocket, stąd 426);
- otwórz `https://twoja.strona/metin2/client.html` i zaloguj się; w dzienniku
  mostu pojawia się `session_opened` i `upstream_connected`;
- `connection_rejected ... unknown_target` w dzienniku znaczy, że gra
  poprosiła o adres, którego most nie dopuszcza - dopisz go do
  `[bridge] targets` w `webclient.toml`, zrób konfigurację ponownie (krok 1)
  i zrestartuj most.
- `curl -I https://twoja.strona/metin2/client.wasm` odpowiada `200`
  z `Content-Type: application/wasm`. `404` zwykle znaczy jedno z trzech:
  na serwerze jest folder `site/` z repozytorium zamiast zawartości
  `dist/site/` (muszą być `client.js`, `client.wasm`, `client.data`
  i `corpus/`); nginx szuka w innym folderze (`root /var/www;` znaczy, że
  pliki muszą leżeć w `/var/www/metin2/`); albo zapytanie przejmuje starszy blok z wyrażeniem regularnym, np.
  `location ~* \.(bin|wasm)$` - takie bloki mają pierwszeństwo przed
  `location /metin2/`. Usuń te pozostałości: oznaczają też
  `corpus/manifest.bin` jako niezmienny, a po aktualizacji gracze
  zostaliby ze starym.
- Most kończy się `EADDRINUSE ... 127.0.0.1:11496`: port trzyma inny
  program (np. wcześniejszy most działający jako usługa).
  `ss -ltnp | grep 11496` pokaże który; zatrzymaj i wyłącz jego usługę.

**Gdy serwer WWW stoi poza siecią serwera gry**

Most łączy się z serwerem gry **z serwera WWW**, pod adres wbudowany
w klienta (`[server] address` w `webclient.toml` i adresy w `serverInfo.py`
klienta). Przeglądarka gracza nigdy nie łączy się z serwerem gry sama. Czyli:

- serwer WWW w tej samej sieci co serwer gry (np. oba w domu): adres
  prywatny typu `192.168.x.x` w kliencie jest w porządku;
- serwer WWW gdzie indziej (VPS, hosting): zbuduj klienta z adresem, który
  SERWER WWW widzi - zwykle publiczne IP albo domena serwera gry - i otwórz
  mu porty logowania i kanałów;
- po wyborze postaci serwer gry odsyła klienta pod adres ze swojej
  konfiguracji. Jeśli to inny host niż ten w kliencie, dopisz go do
  `[bridge] targets` i zrób konfigurację ponownie - inaczej w dzienniku mostu
  pojawi się `unknown_target`.

`bridge_config.py --site` ostrzega, gdy strona jest na publicznym hoście,
a adresy serwera gry są prywatne.

**Uwagi**

- Maszyna z mostem musi widzieć porty serwera gry (zapora).
- Na serwerze z Windows to samo działa z nginx dla Windows; most utrzymasz
  Harmonogramem zadań („przy uruchomieniu”) albo nakładką usługową, np. NSSM.
- Adres serwera gry w kliencie pochodzi z budowy (`[server] address`
  w `webclient.toml`).

## 4. Pasmo i pierwszy start

Korpus ma ~2 GB; nagrana ścieżka startowa (logowanie + wioska) to 97
kawałków ≈ 400 MB. Kolejność zdarzeń w świeżej przeglądarce:

1. `client.js/.wasm/.data` (~50 MB) — strona pokazuje postęp silnika.
2. Wyprzedzenie z `order.txt`: kawałki w kolejności, w jakiej gra o nie
   poprosi, do pamięci (okno 96-512 MB, wg pamięci urządzenia) i Cache
   Storage. Pasek w rogu:
   „Essential data".
3. Gra startuje i prosi o kawałki synchronicznie; czego wyprzedzenie
   jeszcze nie zdążyło, idzie z sieci (jeden kawałek naraz, klatka czeka —
   przy 15 Mbit/s uploadu kawałek 4 MB to ~2 s).
4. W tle: pozostałe kawałki pojedynczo do Cache Storage („Game data
   (background): N / całość"); 150 ms przerwy między kawałkami zostawia pasmo grze.
5. Przy następnym starcie wyprzedzenie czyta ścieżkę startową z Cache
   Storage (dysk) do pamięci, zanim gra o nią poprosi. To, o co gra i tak
   prosi synchronicznie (zmierzone: 22 kawałki przy budżecie 512 MB, 138 przy
   96 MB), przychodzi z pamięci podręcznej HTTP przeglądarki, jeśli serwer
   wysyła `Cache-Control: public, max-age=31536000, immutable` dla
   `corpus/*.bin` (zmierzone: 21 z 22), inaczej z sieci. Żądania
   synchroniczne NIE przechodzą przez Service Workera — Chromium ich do niego
   nie kieruje; najwięcej daje budżet pamięci kawałków
   (`?corpusMB=`, domyślnie wg pamięci urządzenia).

Jeśli Service Worker nie może się zarejestrować (http albo przeglądarka go
blokuje), gra działa nadal; samo Cache Storage wymaga tylko bezpiecznego
pochodzenia (https albo localhost), więc wyprzedzenie z dysku działa i bez
workera.

Orientacyjnie przy 15 Mbit/s (≈1,9 MB/s): silnik ~30 s, ścieżka startowa
~3,5 min, cały korpus ~18 min w tle — raz na przeglądarkę.

## 5. Strona „Zagraj w przeglądarce"

Otwieraj klienta w **nowej karcie** ze swojej strony:

```
<a href="https://twoja.strona/metin2/client.html?deflang=pl&scale=1.5&return=https://twoja.strona" target="_blank">
```

- `scale=` — skala GUI wybrana przez gracza (1, 1.25, 1.5, 1.75, 2, 2.5, 3);
  pamiętana w localStorage, więc formularz ustawień może ją ustawić raz.
- `return=` — dokąd idzie „Wyjdź z gry", gdy karty nie otworzył skrypt
  i nie może się sama zamknąć.
- `deflang=` — język klienta.
- głośność: jeszcze nie jako parametr (okno opcji w grze działa; parametr
  adresu jest na liście).

## 6. Lista kontrolna

- [ ] HTTPS na stronie, `wss://` dochodzi do mostu (`curl -i https://twoja.strona/to/ping` odpowiada `426 Upgrade Required`)
- [ ] `corpus/manifest.bin` i jeden kawałek pobierają się z nagłówkami jak wyżej
- [ ] konsola F12: `[sw] registered`, a później `[corpus] background: ... downloaded`
- [ ] drugi start: `m2w.corpus.fetchCount` małe, kawałki w milisekundach
