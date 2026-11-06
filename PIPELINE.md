# Pipeline do módulo Atrium One

Guia de implementação do software do módulo (Raspberry Pi 3B, hostname
`pi3b`). Define **o que fazer, em que ordem e o que não fazer**.

Os formatos (endpoints, campos, arquivos) estão em
[`CONTRATO.md`](CONTRATO.md), que é a fonte da verdade. Se este
documento e o contrato divergirem, vale o contrato. Se algo não estiver em
nenhum dos dois, **pergunte antes de inventar**.

## 1. Quem faz o quê

| Parte | Onde roda | Responsabilidade |
|---|---|---|
| **Módulo** | Raspberry Pi (`pi3b`) | Sensores, calibração, máquina de estados da missão, captura RGB + NIR, histórico de posições, fila local e envio à estação. |
| **API da estação** | Estação (`station:5000`) | Recebe missões e capturas, alinha o NIR, calcula NDVI, detecta focos, georreferencia e gera prévias. |
| **Dashboard** | Estação | Faz polling de `GET /status`, envia os comandos e mostra tudo. |

O módulo é a **única** parte a implementar neste repositório. A API da
estação e o dashboard já estão prontos, em outro repositório, e seguem o
contrato.

## 2. Fora do escopo do módulo

Estas coisas **não** devem existir no código do módulo, mesmo que pareçam
úteis:

- **Nenhum processamento de imagem.** Sem NDVI, sem alinhamento RGB/NIR, sem
  recorte, sem correção de cor, sem matriz de alinhamento. Sem redimensionar,
  exceto para reduzir o quadro inteiro quando o modo da câmera recortaria o
  sensor (§3.4).
  O módulo manda os quadros **crus**, só codificados em JPEG. (O NDVI no Pi
  foi abandonado: o Pi 3B não aguenta.)
- **Nenhuma análise.** Sem detecção de focos, sem estatísticas e sem
  conversão para latitude/longitude. A posição é **relativa ao ponto de
  partida**, em metros norte/leste.
- **Nenhum endpoint, campo ou estado fora do contrato.** Se precisar de algo
  novo, pare e peça a mudança do contrato.
- **Nenhuma interface web, banco de dados ou autenticação** no módulo. A
  fila é o sistema de arquivos.

## 3. Uma missão, passo a passo

```
 boot ─▶ idle ─▶ (calibrating ─▶ idle) ─▶ in_mission ─▶ uploading ─▶ idle
                                           │ 120 s aquecendo
                                           │ foto a cada ≥10 s, se estável
                                           │ histórico de posições 1–5 Hz
                                           │ envia fotos sempre que alcança a estação
                                           └ ready_to_finish ao voltar
```

### 3.0 Boot

1. Tudo sobe sozinho com o Pi (systemd): API em `0.0.0.0:8000`, sensores e
   câmeras.
2. O estado inicial é `idle`, ou o estado salvo em disco se o módulo
   reiniciou no meio de algo (ver 3.7).
3. `GET /status` responde **sempre**, mesmo com um sensor ou câmera falhando.
   A falha aparece em `devices` (`connected: false` ou `status: "error"` com
   `message`); a API não pode cair por causa disso.

### 3.1 Calibração (opcional, só em `idle`)

1. `POST /calibrate` → estado `calibrating`. A calibração passa pelas
   etapas de `status.calibration.step`:
   - `still`: deixar parado (giroscópio);
   - `figure8`: fazer um oito (magnetômetro);
   - `poses`: apoiar em várias faces (acelerômetro).

   Em cada etapa, `instruction` traz o texto que o dashboard mostra ao
   operador.
2. Ao terminar, o módulo grava a calibração **em disco**, atualiza
   `sensors.calibrated` e `sensors.calibrated_at` e volta para `idle`.
3. Daí em diante, todas as leituras (posição, orientação, altura) já saem
   corrigidas pela calibração. A estação não corrige nada.

### 3.2 Início — `POST /mission/start` (só em `idle`)

1. Gera o `mission_id` (UUID v4).
2. **Zera o referencial**: posição norte/leste = 0 e altura = 0 (acelerômetro
   zerado). O ponto de partida é a estação.
   - A posição horizontal vem do filtro de Kalman (GPS + IMU). O módulo guarda
     a posição GPS do início como origem e converte as leituras seguintes
     para metros norte/leste a partir dela. Para as distâncias de uma missão,
     uma projeção local plana basta.
   - O GPS (NEO-6M) é instável e às vezes perde o fix. Sem fix, a posição sai
     só da IMU (dead reckoning) e deriva. Isso é aceitável, desde que seja
     **declarado**: `position.source` = `imu` no `metadata.json` e na
     coluna `source` do `track.csv`, com `sigma_m` quando houver estimativa.
     Como ancorar a origem quando o fix só chega depois do início é decisão
     do módulo; a estação só exige metros norte/leste a partir do ponto de
     partida e um `source` honesto.
   - A latitude/longitude **não** vai no contrato: a estação só recebe
     metros norte/leste.
3. Registra `started_at` e define `captures_enabled_at` = `started_at` + 120 s.
4. Começa a gravar o **histórico de posições** (`t_s,north_m,east_m,height_m`,
   1 a 5 Hz) num arquivo em disco, não só em memória.
5. Muda para `in_mission` e responde `200 {"mission": …}`.

### 3.3 Aquecimento — primeiros 120 s

Nenhuma foto. O histórico de posições já está gravando.

### 3.4 Capturas

Depois de `captures_enabled_at`:

1. A cada **≥ 10 s** desde a última foto, o módulo fica pronto para a
   próxima.
2. Pronto, ele fotografa **assim que estiver estável**. Sugestão de critério:
   velocidade angular abaixo de um limiar (ex.: 0,15 rad/s) durante ~0,5 s.
   Deixe os limiares como constantes de configuração.
3. Cada foto:
   1. novo UUID (`entry_id`) e `sequence` = 1, 2, 3…;
   2. lê posição, altura e orientação **no instante da foto**. A orientação
      é a do **referencial da câmera RGB** (x = topo da imagem, y = direita,
      z = eixo óptico), não a do drone, porque o módulo pode ser montado em
      qualquer posição;
   3. captura RGB e NIR **ao mesmo tempo** (por exemplo, uma thread por
      câmera, descartando antes os quadros antigos do buffer da câmera USB)
      e grava `rgb.jpg` e `nir.jpg`. A NIR é codificada em JPEG com
      qualidade ≥ 90. A RGB vai como o MJPEG que a câmera produz, **sem
      recodificar** (a Arducam não tem controle de qualidade, e recodificar
      só perderia mais informação). A resolução pode
      ser reduzida para aliviar o Pi, desde que seja o **quadro inteiro**:
      de preferência um modo nativo da câmera que cubra o sensor todo
      (binning); só quando esse modo não existir, redimensionar por software
      o quadro completo. Nunca um modo que recorte o sensor;
   4. escreve o `metadata.json` exatamente como no contrato (seção 3), com
      `schema: 2` e todos os campos obrigatórios (`null` se faltar leitura);
   5. grava tudo numa pasta temporária e **renomeia atomicamente** para a fila
      só quando os três arquivos estiverem completos.

### 3.4.1 Envio durante a missão

Sempre que alcançar a estação (`GET /health` ok), o módulo envia as fotos
da fila com `POST /entry` e apaga cada uma **assim que confirmada**. Não
precisa esperar a missão acabar, nem o `POST /mission`: a estação liga cada
foto à missão pelo `mission_id` do `metadata.json`.

- O envio roda em segundo plano, com prioridade menor que a captura. Ele
  **nunca** pode atrasar uma foto nem a resposta de `GET /status`. No
  Pi 3B, isso significa thread ou processo separado, uma foto por vez.
- Perder a estação no meio de um envio é normal em voo: o item fica na fila
  e vai na próxima oportunidade (mesma regra de falhas da 3.6).
- `status.upload` vale também em `in_mission`: `total` = fotos tiradas até
  agora, `sent` = confirmadas, `pending` = na fila.

Layout sugerido no Pi:

```
data/
  state.json                     estado atual (missão, calibração), para sobreviver a reboot
  calibration/…                  calibração persistida
  tmp/<entry_id>/                captura em escrita
  missions/<mission_id>/
    track.csv                    histórico de posições (append)
    mission.json                 escrito ao finalizar
    queue/<entry_id>/            rgb.jpg, nir.jpg, metadata.json (prontos para envio)
```

### 3.5 Retorno — `ready_to_finish`

Durante toda a missão, o módulo reavalia continuamente (pode voltar a
`false`). O critério depende de haver fix GPS:

| Com fix GPS: todas valem | Sem fix GPS: todas valem |
|---|---|
| Distância horizontal ao ponto inicial < 10 m | — (a posição por IMU não é confiável) |
| `height_m` < 2 m | — |
| Parado (velocidade < 0,3 m/s) há ≥ 3 s | **Pousado**: parado e sem vibração de motor há ≥ 3 s |
| `GET http://station:5000/health` respondeu 200 nos últimos ~5 s | `GET http://station:5000/health` respondeu 200 nos últimos ~5 s |

Os limiares são sugestões e devem ficar configuráveis.

O módulo **não** finaliza sozinho: ele só reporta `ready_to_finish`, e o
operador clica em "Finalizar missão" no dashboard.

### 3.6 Finalização — `POST /mission/finish`

Aceito com `ready_to_finish = true`, ou com `?force=true` (marca
`forced: true`).

1. Para de fotografar e de gravar o histórico. Registra `finished_at`.
2. Escreve `mission.json` (contrato, seção 2) e muda para `uploading`.
   Responde 200.
3. Em segundo plano, **nesta ordem**:
   1. `POST /entry` para cada captura que ainda estiver na fila, em ordem de
      `sequence`. Cada item local é apagado **só depois** da confirmação
      (201 ou 200 com o mesmo id);
   2. por último, `POST /mission` com `mission.json` e o `track.csv`
      completo;
   3. mantém `status.upload` atualizado (`total`, `sent`, `pending`).
4. Fila vazia e missão registrada → `idle`, `mission: null`.

Falhas no envio:

| Situação | O que fazer |
|---|---|
| Estação inacessível ou timeout | Mantém os arquivos, espera e tenta de novo (backoff até ~30 s). Continua em `uploading`. |
| `5xx` | Mesma coisa: tentar de novo depois. |
| `400` em `/entry` ou `/mission` | É bug do módulo. Registra o erro no log, **não apaga** o item, move para `data/rejected/` e segue com os próximos. |
| `200 already_exists` | Sucesso: apaga o item local. |

### 3.7 Reboot no meio do caminho

O estado é salvo em `data/state.json` a cada transição.

- **Em `in_mission`:** volta em `in_mission`, com o mesmo `mission_id`, e
  continua o `track.csv` (`t_s` contado a partir de `started_at`). Os itens
  da fila seguem sendo enviados como na 3.4.1.
- **Em `uploading`:** volta em `uploading` e continua de onde parou
  (fotos restantes, depois `POST /mission`).
- **Em `idle`, mas com itens de uma missão já finalizada na fila:** volta em
  `uploading` até terminar.
- **Em `calibrating`:** volta em `idle`. A calibração anterior continua
  valendo.

## 4. Relógio

O Pi 3B **não tem relógio de hardware**.

- Os timestamps ISO (`started_at`, `timestamp`, `finished_at`,
  `calibrated_at`) vêm do relógio do sistema, sincronizado por NTP quando o
  Pi está com rede.
- Intervalos (10 s entre fotos, 120 s de aquecimento, `t_s` do histórico,
  "parado há 3 s") usam **relógio monotônico**, nunca o do sistema.
- Se o relógio não estiver sincronizado (`timedatectl show -p NTPSynchronized`),
  avise em `devices`, por exemplo
  `{"id": "clock", "label": "Relógio", "connected": true, "status": "warning", "message": "sem NTP"}`.

## 5. Números de referência

| Item | Valor |
|---|---|
| API do módulo | `0.0.0.0:8000`, acessada como `http://pi3b:8000` |
| API da estação | `http://station:5000` |
| Polling do dashboard | a cada 2 s; cada tentativa espera até 2,5 s. `/status` deve responder em < 200 ms |
| Aquecimento | 120 s |
| Intervalo mínimo entre fotos | 10 s |
| Histórico de posições | 1 a 5 Hz |
| Imagens | RGB **1600×1200** (Arducam USB, MJPEG nativo) e NIR **1640×1232** (Pi Camera V2, binning 2×2), ambas com o campo de visão inteiro do sensor. |

## 6. Critérios de aceitação

Com a estação e o dashboard rodando (`station:5000`), confira:

1. `curl http://pi3b:8000/status` responde em < 200 ms com `state`, `devices`
   (pelo menos `imu`, `mag`, `camera_rgb`, `camera_nir`) e `sensors`.
   Desconectar uma câmera não derruba a API; o item aparece com
   `connected: false`.
2. O dashboard mostra "Módulo conectado". Ao parar o serviço do módulo, ele
   mostra "Procurando o módulo…"; ao religar, reconecta sozinho.
3. "Calibrar sensores" faz `calibrating` e depois `idle`, com
   `calibrated_at` novo, que **sobrevive a um reboot** do Pi.
4. "Iniciar missão" cria a missão; nenhuma foto sai antes de
   `captures_enabled_at`, e as fotos ficam espaçadas ≥ 10 s.
5. `POST /calibrate` durante a missão retorna 409 com `detail`.
6. Ao voltar, parado e perto da estação, `ready_to_finish` vira `true` e o
   botão "Finalizar missão" é liberado.
7. Depois de finalizar, a estação tem:
   - `missions/<mission_id>/mission.json` e `track.csv`;
   - uma pasta `entries/<entry_id>/` por foto, com `rgb.jpg`, `nir.jpg` e
     `metadata.json`;
   - em seguida, `analysis.json` e **nenhum** `error.json`.

   O dashboard mostra o trajeto com as fotos sobre ele.
8. Desligando a estação no meio do envio, nada se perde: o módulo segue em
   `uploading` e termina quando ela volta.
9. Reiniciando o Pi no meio do envio, ele volta em `uploading` e completa.
10. Durante a missão, ao alcance da estação, as fotos aparecem em
    `entries/` **antes** de finalizar, somem da fila do Pi após a
    confirmação e `status.upload.sent` sobe em `in_mission`. Com o envio
    ativo, o espaçamento entre fotos continua ≥ 10 s e `GET /status` segue
    respondendo em < 200 ms.

Para testar a ingestão sem o fluxo inteiro:

```bash
curl -F entry_id=<uuid> -F rgb=@rgb.jpg -F nir=@nir.jpg -F metadata=@metadata.json http://station:5000/entry
curl -F mission_id=<uuid> -F mission=@mission.json -F track=@track.csv http://station:5000/mission
```

## 7. Quando parar e perguntar

- Um campo do contrato não dá para preencher com o hardware real.
- O critério de estabilidade ou de retorno precisa de um sensor que não
  existe no hardware.
- Qualquer mudança em endpoint, campo, estado ou formato de arquivo.
