import type { EntrySummary, Severity } from "./types";

// Fotos vizinhas se sobrepõem: o mesmo foco físico aparece em mais de uma.
// Agrupa detecções próximas (no referencial da missão) em um foco único.

export type HotspotCluster = {
  key: string;
  north_m: number | null;
  east_m: number | null;
  severity: Severity;
  ndvi_drop: number;
  area_m2: number | null;
  best: { entryId: string; id: number; sequence: number | null };
  refs: { entryId: string; id: number; sequence: number | null }[];
  uncertain: boolean; // todas as fotos que viram o foco tinham posição só por IMU
};

const RANK: Record<Severity, number> = { alta: 3, media: 2, baixa: 1 };
const MERGE_RADIUS_M = 2.5;

export function clusterHotspots(entries: EntrySummary[]): HotspotCluster[] {
  const detections = entries
    .flatMap((e) => e.hotspots.map((h) => ({ ...h, entryId: e.id, sequence: e.sequence, imu: e.position?.source === "imu" })))
    .sort((a, b) => RANK[b.severity] - RANK[a.severity] || b.ndvi_drop - a.ndvi_drop);

  const clusters: (HotspotCluster & { sumN: number; sumE: number; n: number })[] = [];
  for (const d of detections) {
    const ref = { entryId: d.entryId, id: d.id, sequence: d.sequence };
    const located = d.north_m !== null && d.east_m !== null;
    const radius = Math.max(MERGE_RADIUS_M, d.area_m2 ? Math.sqrt(d.area_m2 / Math.PI) * 1.5 : 0);
    const match = located
      ? clusters.find((c) => c.n > 0 && Math.hypot(c.sumN / c.n - d.north_m!, c.sumE / c.n - d.east_m!) <= radius)
      : undefined;
    if (match) {
      match.refs.push(ref);
      match.sumN += d.north_m!;
      match.sumE += d.east_m!;
      match.n += 1;
      match.north_m = match.sumN / match.n;
      match.east_m = match.sumE / match.n;
      match.area_m2 = Math.max(match.area_m2 ?? 0, d.area_m2 ?? 0) || null;
      match.uncertain = match.uncertain && d.imu;
      continue;
    }
    clusters.push({
      key: `${d.entryId}:${d.id}`,
      north_m: d.north_m,
      east_m: d.east_m,
      severity: d.severity,
      ndvi_drop: d.ndvi_drop,
      area_m2: d.area_m2,
      best: ref,
      refs: [ref],
      uncertain: d.imu,
      sumN: located ? d.north_m! : 0,
      sumE: located ? d.east_m! : 0,
      n: located ? 1 : 0,
    });
  }
  return clusters.map(({ sumN, sumE, n, ...c }) => c);
}

// Área coberta pela união das pegadas (rasterização simples em metros).
export function coveredArea(footprints: [number, number][][]) {
  if (!footprints.length) return 0;
  const all = footprints.flat();
  const minN = Math.min(...all.map((p) => p[0])), maxN = Math.max(...all.map((p) => p[0]));
  const minE = Math.min(...all.map((p) => p[1])), maxE = Math.max(...all.map((p) => p[1]));
  const cell = Math.max(0.5, Math.sqrt(((maxN - minN) * (maxE - minE)) / 250_000));
  const rows = Math.ceil((maxN - minN) / cell), cols = Math.ceil((maxE - minE) / cell);
  const covered = new Uint8Array(rows * cols);

  for (const poly of footprints) {
    const pn = poly.map((p) => p[0]), pe = poly.map((p) => p[1]);
    const r0 = Math.max(0, Math.floor((Math.min(...pn) - minN) / cell)), r1 = Math.min(rows - 1, Math.floor((Math.max(...pn) - minN) / cell));
    const c0 = Math.max(0, Math.floor((Math.min(...pe) - minE) / cell)), c1 = Math.min(cols - 1, Math.floor((Math.max(...pe) - minE) / cell));
    for (let r = r0; r <= r1; r++) {
      const n = minN + (r + 0.5) * cell;
      for (let c = c0; c <= c1; c++) {
        const e = minE + (c + 0.5) * cell;
        let inside = false;
        for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
          const [ni, ei] = poly[i], [nj, ej] = poly[j];
          if ((ni > n) !== (nj > n) && e < ((ej - ei) * (n - ni)) / (nj - ni) + ei) inside = !inside;
        }
        if (inside) covered[r * cols + c] = 1;
      }
    }
  }
  let count = 0;
  for (const v of covered) count += v;
  return count * cell * cell;
}
