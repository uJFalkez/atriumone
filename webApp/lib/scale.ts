// Escala divergente do NDVI. Mesmos pontos de server/processing.py (_NDVI_STOPS).
export const NDVI_STOPS: [number, [number, number, number]][] = [
  [-1.0, [92, 38, 18]],
  [-0.6, [166, 78, 38]],
  [-0.3, [214, 139, 92]],
  [0.0, [232, 228, 218]],
  [0.25, [150, 205, 176]],
  [0.5, [60, 160, 120]],
  [0.75, [22, 112, 86]],
  [1.0, [8, 70, 56]],
];

export function ndviRgb(value: number): [number, number, number] {
  const v = Math.max(-1, Math.min(1, value));
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

export const ndviGradientCss = `linear-gradient(90deg, ${NDVI_STOPS.map(([x, c]) => `rgb(${c.join(",")}) ${((x + 1) / 2) * 100}%`).join(", ")})`;

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
