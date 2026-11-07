// Escala divergente do NDVI. Mesmos pontos de server/processing.py (_NDVI_STOPS).
// Só exibição: o pseudo-NDVI (NoIR sem filtro passa-IR) fica comprimido perto de
// zero, então a escala cobre -0,2..+0,6 (neutro em +0,2) e satura nas pontas.
export const NDVI_DISPLAY_MIN = -0.2;
export const NDVI_DISPLAY_MAX = 0.6;
export const NDVI_STOPS: [number, [number, number, number]][] = [
  [-0.2, [92, 38, 18]],
  [-0.04, [166, 78, 38]],
  [0.08, [214, 139, 92]],
  [0.2, [232, 228, 218]],
  [0.3, [150, 205, 176]],
  [0.4, [60, 160, 120]],
  [0.5, [22, 112, 86]],
  [0.6, [8, 70, 56]],
];

export function ndviRgb(value: number): [number, number, number] {
  const v = Math.max(NDVI_DISPLAY_MIN, Math.min(NDVI_DISPLAY_MAX, value));
  for (let i = 1; i < NDVI_STOPS.length; i++) {
    const [x1, c1] = NDVI_STOPS[i];
    const [x0, c0] = NDVI_STOPS[i - 1];
    if (v <= x1) {
      const t = (v - x0) / (x1 - x0);
      return c0.map((c, k) => Math.round(c + (c1[k] - c) * t)) as [number, number, number];
    }
  }
  return NDVI_STOPS.at(-1)![1];
}

export const ndviColor = (value: number) => `rgb(${ndviRgb(value).join(",")})`;

export const ndviGradientCss = `linear-gradient(90deg, ${NDVI_STOPS.map(([x, c]) => `rgb(${c.join(",")}) ${((x - NDVI_DISPLAY_MIN) / (NDVI_DISPLAY_MAX - NDVI_DISPLAY_MIN)) * 100}%`).join(", ")})`;

// Expressão MapLibre equivalente.
export const ndviMapExpression = (input: unknown) => [
  "interpolate", ["linear"], input,
  ...NDVI_STOPS.flatMap(([x, c]) => [x, `rgb(${c.join(",")})`]),
];

// Paleta de status (fixa): sempre acompanhada de ícone + rótulo.
export const SEVERITY = {
  alta: { color: "#d03b3b", label: "Alta" },
  media: { color: "#ec835a", label: "Média" },
  baixa: { color: "#fab219", label: "Baixa" },
} as const;
