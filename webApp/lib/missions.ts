import { mkdir, readdir, readFile, rename, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { isUuid, missionsDirectory } from "./entries";
import type { MissionRecord, TrackStats } from "./types";

// missions/<uuid>/
//   record.json   registro do dashboard (comandos de início/fim)
//   mission.json  registro enviado pelo módulo (POST /mission na estação)
//   track.csv     histórico de posições: t_s,north_m,east_m[,…]

type Json = Record<string, unknown>;

async function readJson(file: string): Promise<Json | null> {
  try {
    const value = JSON.parse(await readFile(file, "utf8"));
    return value && typeof value === "object" && !Array.isArray(value) ? value : null;
  } catch {
    return null;
  }
}

async function mtime(file: string) {
  try {
    return (await stat(file)).mtimeMs;
  } catch {
    return 0;
  }
}

const str = (v: unknown) => (typeof v === "string" ? v : null);
const num = (v: unknown) => (typeof v === "number" && Number.isFinite(v) ? v : null);

// ===== Trajeto =====

// [north_m, east_m, t_s, confiável (1) ou só IMU (0)]
export type TrackPoint = [number, number, number, number];
export type Track = { points: TrackPoint[]; stats: TrackStats };

const trackCache = new Map<string, { key: number; track: Track | null }>();

export async function readTrack(id: string): Promise<Track | null> {
  if (!isUuid(id)) return null;
  const file = path.join(missionsDirectory, id, "track.csv");
  const key = await mtime(file);
  if (!key) return null;
  const cached = trackCache.get(id);
  if (cached?.key === key) return cached.track;

  let track: Track | null = null;
  try {
    const lines = (await readFile(file, "utf8")).split(/\r?\n/).filter((l) => l.trim());
    const header = lines[0].split(",").map((c) => c.trim());
    const [it, inn, ie, isrc] = ["t_s", "north_m", "east_m", "source"].map((c) => header.indexOf(c));
    const points: TrackPoint[] = [];
    for (const line of lines.slice(1)) {
      const cells = line.split(",");
      const reliable = isrc >= 0 && cells[isrc]?.trim() === "imu" ? 0 : 1;
      const p: TrackPoint = [Number(cells[inn]), Number(cells[ie]), Number(cells[it]), reliable];
      if (p.every(Number.isFinite)) points.push(p);
    }
    if (points.length >= 2) {
      let distance = 0, range = 0;
      points.forEach(([n, e], i) => {
        range = Math.max(range, Math.hypot(n, e));
        if (i) distance += Math.hypot(n - points[i - 1][0], e - points[i - 1][1]);
      });
      track = {
        points,
        stats: {
          samples: points.length,
          distance_m: distance,
          max_range_m: range,
          duration_s: points.at(-1)![2] - points[0][2],
          imu_fraction: points.filter((p) => !p[3]).length / points.length,
        },
      };
    }
  } catch {}
  trackCache.set(id, { key, track });
  return track;
}

// Reduz o trajeto para desenhar (mantém início e fim).
export function simplifyTrack(points: TrackPoint[], max = 2500) {
  if (points.length <= max) return points;
  const stride = (points.length - 1) / (max - 1);
  const kept = new Set(Array.from({ length: max }, (_, i) => Math.round(i * stride)));
  // Preserva as trocas GPS ↔ IMU para os trechos não "vazarem" de cor.
  points.forEach((p, i) => i && p[3] !== points[i - 1][3] && (kept.add(i - 1), kept.add(i)));
  return [...kept].sort((a, b) => a - b).map((i) => points[i]);
}

// ===== Registro =====

export async function readMissions(): Promise<MissionRecord[]> {
  let folders: string[];
  try {
    folders = (await readdir(missionsDirectory, { withFileTypes: true })).filter((f) => f.isDirectory() && isUuid(f.name)).map((f) => f.name);
  } catch {
    return [];
  }
  const records = await Promise.all(
    folders.map(async (id): Promise<MissionRecord | null> => {
      const dir = path.join(missionsDirectory, id);
      const [record, module, track] = await Promise.all([readJson(path.join(dir, "record.json")), readJson(path.join(dir, "mission.json")), readTrack(id)]);
      if (!record && !module) return null;
      // O registro do módulo é a fonte da verdade; o do dashboard completa.
      return {
        id,
        started_at: str(module?.started_at) ?? str(record?.started_at),
        finished_at: str(module?.finished_at) ?? str(record?.finished_at),
        forced: module?.forced === true || record?.forced === true || undefined,
        module_id: str(module?.module_id) ?? str(record?.module_id),
        registered: !!module,
        captures: num(module?.captures),
        track: track?.stats ?? null,
      };
    }),
  );
  return records.filter((r): r is MissionRecord => !!r);
}

export async function upsertMission(id: string, patch: Partial<MissionRecord>) {
  if (!isUuid(id)) return;
  const file = path.join(missionsDirectory, id, "record.json");
  const current = (await readJson(file)) || { id };
  const next = { ...current, ...Object.fromEntries(Object.entries(patch).filter(([, v]) => v !== undefined)), id };
  if (JSON.stringify(next) === JSON.stringify(current)) return;
  await mkdir(path.dirname(file), { recursive: true });
  const temporary = `${file}.${process.pid}.tmp`;
  await writeFile(temporary, JSON.stringify(next, null, 2));
  await rename(temporary, file);
}
