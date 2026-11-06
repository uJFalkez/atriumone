# Contrato módulo ↔ estação

Este documento define o que o módulo (Raspberry Pi) e a estação trocam entre si.
A estação tem duas partes:
- a API de ingestão (Flask, porta 5000);
- o dashboard (Next.js), que comanda o módulo e mostra os dados.

```
            comandos + polling de status
 dashboard ───────────────────────────────▶ API do módulo (:8000)
 (webApp)                                        │
                                                 │ POST /entry: a qualquer momento, sempre
                                                 │   que alcançar a estação (inclusive em voo)
 API da estação (:5000) ◀────────────────────────┘ POST /mission: uma vez, no fim, por último
   ├─ missions/<uuid>/  mission.json, track.csv
   └─ entries/<uuid>/   processa: alinhamento, NDVI, focos
```

## Rede

As duas APIs conversam pela tailnet (Tailscale), usando os hostnames
`pi3b` (módulo, porta 8000) e `station` (estação, porta 5000). Os dois
serviços precisam escutar em `0.0.0.0` ou no IP do Tailscale, não só em
`127.0.0.1`.

## 1. API do módulo (a estação chama)

Base: `http://pi3b:8000`, o hostname do Raspberry na tailnet (configurável no dashboard por `MODULE_URL`).
Erros usam HTTP 4xx/5xx com corpo `{"detail": "mensagem legível"}`. O
dashboard mostra o `detail` ao operador.

### `GET /status`

É o polling do dashboard e a única fonte de status do módulo: conexão,
estado, dispositivos, calibração, missão e envio.

- O dashboard consulta a cada **2 s, sem parar**. Se o módulo não responde,
  a interface mostra "procurando o módulo" e continua tentando até ele
  aparecer na rede. Não existe estado de "desistiu".
- Cada tentativa espera no máximo **2,5 s**, só para o laço não travar.
  "Módulo conectado" significa que a última tentativa respondeu com 200.
- A resposta precisa ser rápida e leve: dados em memória, nada de acessar
  câmera nem fazer I/O pesado dentro da requisição.

```json
{
  "module_id": "atrium-one-01",
  "version": "1.2.0",
  "uptime_s": 5321,
  "state": "in_mission",
  "devices": [
    { "id": "imu", "label": "IMU · MPU6500", "connected": true, "status": "ok", "data": { "taxa": "100 Hz" } },
    { "id": "gps", "label": "GPS · NEO-6M", "connected": true, "status": "warning", "message": "sem fix", "data": { "satélites": 3 } },
    { "id": "mag", "label": "Mag · QMC5883P", "connected": true },
    { "id": "camera_rgb", "label": "RGB · Arducam 8MP USB", "connected": true },
    { "id": "camera_nir", "label": "NIR · Pi Camera V2 NoIR", "connected": false, "message": "não detectada" }
  ],
  "sensors": {
    "calibrated": true,
    "calibrated_at": "2026-11-06T13:58:10+00:00"
  },
  "calibration": null,
  "mission": {
    "id": "6f1c…",
    "started_at": "2026-11-06T14:00:00+00:00",
    "captures_enabled_at": "2026-11-06T14:02:00+00:00",
    "captures": 7,
    "last_capture_at": "2026-11-06T14:05:31+00:00",
    "ready_to_finish": false
  },
  "upload": { "pending": 0, "sent": 0, "total": 0 },
  "live": { "north_m": 41.2, "east_m": -12.8, "height_m": 35.0, "yaw_deg": 87.5 }
}
```

| Campo | Regras |
|---|---|
| `state` | `idle` · `calibrating` · `in_mission` · `uploading` |
| `version`, `uptime_s` | Opcionais. Versão do software do módulo e segundos desde que ligou. |
| `devices` | Lista dos dispositivos do módulo, na ordem em que devem aparecer. Ids conhecidos pelo dashboard: `imu`, `gps`, `mag`, `camera_rgb`, `camera_nir`; outros ids também são exibidos. Campos de cada item: `id` (obrigatório), `connected` (obrigatório), `label` (nome com modelo), `status` (`ok` · `warning` · `error`; se omitido, vale `ok` quando conectado), `message` (motivo curto do aviso) e `data` (até ~3 leituras curtas, `{nome: valor}`). |
| `sensors` | `calibrated` indica se existe calibração válida. `calibrated_at` é a hora da **última calibração** concluída (ISO 8601), mantida entre reinícios. |
| `calibration` | Presente só em `calibrating`: `{ "started_at", "step", "instruction", "remaining_s" \| null, "progress" (0–1) \| null }`. `step` é a etapa atual: `still` (deixar parado, giroscópio), `figure8` (oito, magnetômetro), `poses` (apoiar em várias faces, acelerômetro) ou `done`. `instruction` é um texto curto em português para o operador (ex.: "Gire o módulo fazendo um oito"); o dashboard mostra esse texto. `progress` e `remaining_s` podem se referir à etapa ou ao total. |
| `mission` | Presente em `in_mission` e `uploading`, `null` nos demais estados. `captures_enabled_at` = início + **120 s**. `ready_to_finish` fica `true` quando o módulo percebe que voltou à estação. **Com fix GPS:** distância horizontal ao ponto inicial < 10 m, altura < 2 m, parado há ≥ 3 s e `GET /health` da estação respondendo. **Sem fix GPS** (a posição por IMU deriva demais): pousado (parado e sem vibração de motor) há ≥ 3 s e `GET /health` da estação respondendo. |
| `upload` | Andamento do envio das fotos da missão atual, válido em `in_mission` e em `uploading`: `total` = fotos tiradas até agora, `sent` = já confirmadas pela estação, `pending` = na fila do módulo. O dashboard mostra o envio também durante o voo. |
| `live` | Opcional. Posição atual no referencial da missão (seção 3). O dashboard usa para desenhar o módulo ao vivo no mapa. |

Estados e transições:

```
idle ──POST /calibrate──▶ calibrating ──(fim)──▶ idle
idle ──POST /mission/start──▶ in_mission ──POST /mission/finish──▶ uploading ──(fila vazia)──▶ idle
```

### `POST /mission/start`

- Aceito só em `idle`.
- Zera o referencial: posição e altura passam a ser medidas a partir daqui.
- Resposta 200: `{"mission": { …mesmo formato de status.mission… }}`.
- Resposta 409 se o módulo estiver ocupado.

### `POST /mission/finish`

- Aceito só em `in_mission` com `ready_to_finish = true`.
- Com `?force=true`, aceita mesmo sem `ready_to_finish`. É a saída de
  emergência que o dashboard oferece com confirmação.
- Ao aceitar, o módulo para de fotografar e passa para `uploading`. Em
  seguida envia à API da estação (seção 2) as capturas que ainda estiverem
  na fila (`POST /entry`) e, **por último**, o registro da missão
  (`POST /mission`). Fila vazia e missão registrada → `idle`.
- Resposta 200: `{"mission": {…}}`. Resposta 409 caso contrário.

### `POST /calibrate`

- Aceito só em `idle`. Recusado durante missão e durante envio.
- Resposta 200: `{"calibration": {…}}`.
- A calibração passa pelas etapas de `status.calibration.step`, cada uma
  com um movimento diferente. O dashboard mostra `instruction` ao operador.
- Ao terminar, o módulo volta para `idle` e atualiza
  `sensors.calibrated_at`.

## 2. API da estação (o módulo chama)

Base: `http://station:5000`, o hostname da estação na tailnet.

### `GET /health` → `{"status": "ok"}`

O módulo usa para saber se tem acesso à estação, uma das condições de
`ready_to_finish`.

### `POST /entry` (multipart/form-data, uma requisição por foto)

Aceito **a qualquer momento**: durante a missão, sempre que o módulo
alcançar a estação, e depois de finalizar. Pode chegar antes do
`POST /mission` daquela missão. A estação grava e processa cada foto de
forma independente, e o dashboard a liga à missão pelo `mission_id` do
`metadata.json`. Não há estado de foto "órfã".

| Campo | Tipo | Conteúdo |
|---|---|---|
| `entry_id` | texto | UUID da captura. É o nome da pasta local e o `id` do metadata. |
| `rgb` | arquivo | `rgb.jpg`, quadro RGB **cru**, sem recorte nem alinhamento (ver resolução abaixo). |
| `nir` | arquivo | `nir.jpg`, quadro NoIR **cru**. |
| `metadata` | arquivo | `metadata.json` (seção 3). |

A resolução pode ser reduzida, desde que seja o **quadro inteiro
reescalado**, sem recorte. Pode ser um modo do sensor que cubra o campo de
visão completo (binning) ou um redimensionamento por software. Modos que
cortam o sensor mudam o campo de visão e invalidam o alinhamento. A estação
lê o tamanho de cada imagem e converte a matriz de alinhamento
automaticamente; RGB e NIR podem ter tamanhos diferentes entre si.

Respostas:
- `201 {"entry_id", "status": "created"}`: recebida.
- `200 {"entry_id", "status": "already_exists"}`: reenvio de algo já gravado. Também conta como confirmação.
- `400 {"error"}`: requisição inválida. Não adianta reenviar igual.

Só depois de um 201 ou 200 com o mesmo `entry_id` o módulo pode apagar a
pasta local. A estação grava os arquivos crus de forma atômica e processa em
segundo plano (alinhamento, NDVI, focos); o módulo não espera o
processamento terminar.

### `POST /mission` (multipart/form-data, uma vez por missão)

Enviado uma única vez, **depois** de `POST /mission/finish` e depois das
capturas que faltavam, com o `track.csv` completo. Várias entries da missão
normalmente já terão chegado antes dele.

Registra a missão na estação junto com o **histórico de posições** (2D).
Até ele chegar, o dashboard desenha o percurso ligando as fotos; depois,
usa o trajeto registrado.
O dashboard desenha esse histórico como o trajeto da missão no mapa, com as
fotos marcadas sobre ele.

| Campo | Tipo | Conteúdo |
|---|---|---|
| `mission_id` | texto | UUID da missão (`status.mission.id`). |
| `mission` | arquivo | `mission.json` (abaixo). |
| `track` | arquivo | `track.csv` (abaixo). |

`mission.json`:

```json
{
  "id": "6f1c…",
  "module_id": "atrium-one-01",
  "started_at": "2026-11-06T14:00:00+00:00",
  "finished_at": "2026-11-06T14:21:40+00:00",
  "captures": 37,
  "forced": false
}
```

| Campo | Regras |
|---|---|
| `id`, `started_at`, `finished_at` | Obrigatórios. `id` igual a `mission_id`; datas em ISO 8601 com fuso. |
| `captures` | Recomendado. Número de fotos da missão; o dashboard compara com as fotos recebidas. |
| `forced` | `true` se a missão foi finalizada com `?force=true`. |
| `module_id` | Opcional. |

`track.csv`: uma linha por leitura de posição, em ordem de tempo.

```csv
t_s,north_m,east_m,height_m
0.0,0.00,0.00,0.0
0.2,0.01,0.00,0.1
0.4,0.03,0.01,0.4
```

| Coluna | Regras |
|---|---|
| `t_s` | Obrigatória. Segundos desde o início da missão; não pode decrescer. |
| `north_m`, `east_m` | Obrigatórias. Mesmo referencial do `metadata.json`. |
| `source` | Opcional. `gps+imu` ou `imu`, como em `metadata.json`. O dashboard desenha os trechos `imu` como pouco confiáveis. |
| `sigma_m` | Opcional. Incerteza horizontal (1σ, metros). |
| outras | Opcionais (ex.: `height_m`). São guardadas e ignoradas pelo dashboard. |

Pelo menos 2 linhas. Uma taxa de 1 a 5 Hz é suficiente para o desenho.

Respostas:
- `201 {"mission_id", "status": "created", "samples"}`.
- `200 {"mission_id", "status": "already_exists"}`: reenvio. Também conta como confirmação.
- `400 {"error"}`: o arquivo foi rejeitado. A mensagem diz a linha ou o campo com problema.

## 3. `metadata.json`

```json
{
  "schema": 2,
  "id": "0b7c2d0e-3f4a-4c55-9d1e-7a0f3c2b9e11",
  "mission_id": "6f1c…",
  "sequence": 7,
  "timestamp": "2026-11-06T14:05:31.204+00:00",
  "position": {
    "north_m": 41.2,
    "east_m": -12.8,
    "height_m": 35.0,
    "source": "gps+imu",
    "sigma_m": 2.5
  },
  "orientation": {
    "quaternion": { "w": 0.71, "x": 0.01, "y": -0.02, "z": 0.70 },
    "roll_deg": 1.2,
    "pitch_deg": -2.4,
    "yaw_deg": 87.5
  },
  "stability": {
    "angular_speed_rad_s": 0.04
  }
}
```

| Campo | Obrigatório | Significado |
|---|---|---|
| `schema` | sim | Sempre `2`. |
| `id` | sim | UUID da captura, igual ao `entry_id`. |
| `mission_id` | sim | UUID da missão, igual a `status.mission.id`. |
| `sequence` | sim | 1, 2, 3… dentro da missão. |
| `timestamp` | sim | ISO 8601 **com fuso** (preferir UTC) do instante da foto. |
| `position.north_m`, `east_m` | sim | Deslocamento horizontal em metros desde o início da missão: norte e leste. |
| `position.height_m` | sim | Altura em metros acima do ponto inicial, positiva para cima. Pode vir do acelerômetro zerado no início; não precisa ser precisa. |
| `position.source` | recomendado | `gps+imu` (com fix) ou `imu` (dead reckoning, pouco confiável). O dashboard sinaliza fotos com `imu`. |
| `position.sigma_m` | opcional | Incerteza horizontal estimada (1σ, metros). |
| `orientation.quaternion` | recomendado | Rotação do **referencial da câmera** para NED (Hamilton; `w` escalar). Ver "Convenções físicas". |
| `orientation.roll_deg`, `pitch_deg`, `yaw_deg` | sim | Euler ZYX do mesmo referencial. `yaw_deg` é o rumo do **topo da imagem**, em graus a partir do norte, sentido horário. Com a câmera no nadir, roll e pitch ficam perto de 0. |
| `stability` | opcional | Leituras que justificaram a foto. |

Campos desconhecidos são aceitos e aparecem no dashboard como "outros dados".
Valores sem leitura vão como `null`; não omita os campos obrigatórios.

### Convenções físicas

- **Referencial:** NED local com origem no ponto de partida da missão, que é
  a estação. O dashboard converte para lat/lon usando a posição da estação,
  configurada nele próprio.
- **Orientação = referencial da câmera RGB.** O módulo pode ser preso ao
  drone em qualquer posição, então a orientação reportada é a da câmera, e
  não a do drone:
  - x = direção do topo da imagem;
  - y = direção da direita da imagem;
  - z = eixo óptico, saindo da lente para a cena.

  Com a câmera apontando para o nadir, isso é FRD. A estação projeta a
  pegada de cada foto no solo com o quaternion (ou roll/pitch/yaw), então
  câmeras fora do nadir também são tratadas.
- **Câmeras:** RGB Arducam 8MP USB (3264×2448) e NIR Pi Camera V2 NoIR
  (3280×2464, HFOV 62,2°). A estação usa o HFOV da câmera **RGB**, estimado em
  ≈ 63,6° a partir da matriz de alinhamento RGB/NIR.
- **Calibração dos sensores:** os valores já chegam corrigidos pela
  calibração (o oito). A estação não aplica nenhuma correção.

## 4. O que a estação gera por entrada

Em `entries/<uuid>/`, além dos três arquivos recebidos:

| Arquivo | Conteúdo |
|---|---|
| `ndvi.npy` | NDVI recortado à região alinhada, `uint8`, com 0 → −1, 127 → 0 e 255 → +1. |
| `analysis.json` | Estatísticas do NDVI, focos de NDVI baixo e pegada da foto no solo (`footprint_ne_m`, em metros N/L). |
| `preview/rgb.jpg`, `preview/ndvi.png`, `preview/thumb.jpg` | Versões leves para o dashboard. |
| `error.json` | Presente só se o processamento falhar; a estação pode reprocessar depois. |
