# Dashboard da estação

## Estrutura

- `app/page.tsx` carrega o estado inicial no servidor (`lib/entries.ts`) e
  entrega ao `Dashboard`.
- `app/components/Dashboard.tsx` guarda o estado da tela e faz dois pollings:
  dados a cada 3 s (`/api/data`, com assinatura para evitar tráfego repetido)
  e módulo a cada 2 s (`/api/module/status`). Sem conexão, mantém o último
  status conhecido para a missão não "sumir" da interface.
- `TopBar` traz o status do módulo e os botões, com estas regras:
  - nada funciona sem conexão;
  - **Calibrar** só funciona com o módulo em `idle`;
  - **Iniciar** vira **Finalizar** durante a missão e só é liberado com
    `ready_to_finish`;
  - **Forçar finalização** fica no menu "⋯" e pede confirmação.
- `MapView` usa MapLibre com o satélite da Esri. As fotos não são desenhadas
  no mapa: a foto aparece (e só então é carregada) no popup do hover, e a
  pegada só é desenhada para a foto sob o mouse ou selecionada. Camadas:
  - trajeto e pontos coloridos pelo NDVI médio;
  - focos agrupados (`lib/hotspots.ts`) com pulso;
  - estação e módulo ao vivo, com o rastro em memória.
- `Inspector` mostra o resumo da missão (KPIs, NDVI por foto, focos
  prioritários) ou o detalhe de uma foto, com estes elementos:
  - comparação RGB/NDVI com cortina e contorno dos focos;
  - histograma;
  - instrumentos de atitude;
  - arquivos para download.

## Rotas

| Rota | Função |
|---|---|
| `GET /api/data?sig=` | Missões, capturas resumidas e posição da estação |
| `GET /api/module/status` | Polling do módulo; sincroniza `missions/` |
| `POST /api/module/{mission/start, mission/finish[?force=true], calibrate}` | Repassa o comando ao módulo |
| `GET/PUT /api/station` | Posição da estação |
| `GET /api/missions/<uuid>/track` | Histórico de posições da missão, reduzido para desenho |
| `GET /api/entries/<uuid>/<arquivo>` | Arquivos crus, prévias e `analysis.json` |

## Convenções

- Só aparecem capturas no formato atual (com `nir.jpg`). Pastas antigas, com
  NDVI calculado no módulo, ficam no disco e são ignoradas.
- Posições chegam em metros N/L a partir da estação; `lib/geo.ts` converte
  para lat/lon.
- A escala do NDVI é divergente: terra ← neutro → verde. É definida em
  `lib/scale.ts` e espelhada em `server/processing.py`.
- Severidade usa a paleta de status fixa e sempre vem com ícone e rótulo.
