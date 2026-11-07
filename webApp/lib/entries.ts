import { createHash } from "node:crypto";
import { mkdir, readdir, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { readMissions } from "./missions";
import type { DataPayload, EntrySummary, HotspotLite, Station } from "./types";

// ===== Caminhos =====

export const entriesDirectory = path.resolve(process.env.ENTRIES_DIR || "../entries");
export const missionsDirectory = path.resolve(process.env.MISSIONS_DIR || path.join(entriesDirectory, "..", "missions"));
// Posição da estação: estado do próprio dashboard (a API de ingestão não usa).
export const stationFile = path.resolve(process.env.STATION_FILE || "station.json");

const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function isUuid(value: string) {
  return uuid.test(value);
}

export function entryFile(id: string, file: string) {
  if (!uuid.test(id)) throw new Error("Identificador inválido.");
  return path.join(entriesDirectory, id, file);
}

// ===== Utilidades =====

type Json = Record<string, unknown>;

async function readJson(file: string): Promise<Json | null> {
  try {
    const value = JSON.parse(await readFile(file, "utf8"));
    return value && typeof value === "object" && !Array.isArray(value) ? value : null;
  } catch {
    return null;
  }
}

async function writeJsonAtomic(file: string, value: unknown) {
  await mkdir(path.dirname(file), { recursive: true });
  const temporary = `${file}.${process.pid}.tmp`;
  await writeFile(temporary, JSON.stringify(value, null, 2));
  await rename(temporary, file);
}

async function mtime(file: string) {
  try {
    return (await stat(file)).mtimeMs;
  } catch {
    return 0;
  }
}

const num = (value: unknown): number | null => (typeof value === "number" && Number.isFinite(value) ? value : null);
const str = (value: unknown): string | null => (typeof value === "string" ? value : null);
const obj = (value: unknown): Json => (value && typeof value === "object" && !Array.isArray(value) ? (value as Json) : {});

// ===== Entradas =====

const cache = new Map<string, { key: string; summary: EntrySummary }>();

async function summarize(id: string): Promise<EntrySummary> {
  const dir = path.join(entriesDirectory, id);
  const [metaTime, analysisTime, errorTime, nirTime] = await Promise.all(
    ["metadata.json", "analysis.json", "error.json", "nir.jpg"].map((f) => mtime(path.join(dir, f))),
  );
  const key = `${metaTime}:${analysisTime}:${errorTime}:${nirTime}`;
  const cached = cache.get(id);
  if (cached?.key === key) return cached.summary;

  const metadata = (await readJson(path.join(dir, "metadata.json"))) || {};
  const analysis = analysisTime ? await readJson(path.join(dir, "analysis.json")) : null;
  const failure = errorTime ? await readJson(path.join(dir, "error.json")) : null;

  const position = obj(metadata.position);
  const orientation = obj(metadata.orientation);
  const hasPosition = num(position.north_m) !== null || num(position.east_m) !== null;

  let status: EntrySummary["status"] = "processing";
  if (analysis) status = "processed";
  else if (failure) status = "error";

  const ndvi = obj(analysis?.ndvi);
  const geo = obj(analysis?.geo);
  const crop = obj(analysis?.crop);
  const hotspots: HotspotLite[] = Array.isArray(analysis?.hotspots)
    ? (analysis.hotspots as Json[]).map((h) => ({
        id: num(h.id) ?? 0,
        north_m: num(h.north_m),
        east_m: num(h.east_m),
        severity: (str(h.severity) as HotspotLite["severity"]) || "baixa",
        ndvi_drop: num(h.ndvi_drop) ?? 0,
        area_m2: num(h.area_m2),
      }))
    : [];

  const summary: EntrySummary = {
    id,
    missionId: str(metadata.mission_id),
    sequence: num(metadata.sequence),
    timestamp: str(metadata.timestamp),
    position: hasPosition
      ? {
          north_m: num(position.north_m),
          east_m: num(position.east_m),
          height_m: num(position.height_m),
          source: position.source === "imu" || position.source === "gps+imu" ? position.source : null,
          sigma_m: num(position.sigma_m),
        }
      : null,
    orientation: Object.keys(orientation).length
      ? { roll_deg: num(orientation.roll_deg), pitch_deg: num(orientation.pitch_deg), yaw_deg: num(orientation.yaw_deg) }
      : null,
    status,
    error: failure ? str(failure.error) || "Falha no processamento." : undefined,
    ndvi: analysis && num(ndvi.mean) !== null
      ? { mean: num(ndvi.mean)!, median: num(ndvi.median) ?? num(ndvi.mean)!, vegetation_fraction: num(ndvi.vegetation_fraction) ?? 0 }
      : null,
    hotspots,
    footprint: Array.isArray(geo.footprint_ne_m) ? (geo.footprint_ne_m as [number, number][]) : null,
    gsdCm: num(geo.gsd_cm),
    areaM2: num(geo.area_m2),
    crop: analysis && num(crop.x2) !== null
      ? { width: num(crop.x2)! - (num(crop.x1) ?? 0), height: num(crop.y2)! - (num(crop.y1) ?? 0) }
      : null,
    version: String(Math.round(analysisTime || errorTime || metaTime)),
    // Sem análise de qualidade (processando/erro), conta como dentro até saber.
    survey: analysis ? obj(analysis.quality).survey !== false : true,
    flags: Array.isArray(obj(analysis?.quality).reasons) ? (obj(analysis?.quality).reasons as string[]) : [],
    alignmentUncertain: obj(analysis?.quality).alignment_uncertain === true,
  };
  cache.set(id, { key, summary });
  return summary;
}

// ===== Estação =====

export async function readStation(): Promise<Station | null> {
  const saved = await readJson(stationFile);
  const latitude = num(saved?.latitude);
  const longitude = num(saved?.longitude);
  if (latitude === null || longitude === null) return null;
  return { latitude, longitude, name: str(saved?.name) };
}

export async function writeStation(station: Station) {
  await writeJsonAtomic(stationFile, station);
}

export async function removeStation() {
  await rm(stationFile, { force: true });
}

// ===== Payload do dashboard =====

export async function loadData(): Promise<DataPayload> {
  let entries: EntrySummary[] = [];
  let error: string | undefined;
  try {
    const folders = await readdir(entriesDirectory, { withFileTypes: true });
    // Só capturas no formato atual (RGB + NIR crus). Pastas antigas, com NDVI
    // calculado no módulo, ficam no disco mas são ignoradas.
    const current = await Promise.all(
      folders
        .filter((f) => f.isDirectory() && uuid.test(f.name))
        .map(async (f) => ((await mtime(path.join(entriesDirectory, f.name, "nir.jpg"))) ? f.name : null)),
    );
    entries = await Promise.all(current.filter((id): id is string => !!id).map((id) => summarize(id)));
  } catch (failure) {
    // Pasta ainda não criada = nenhuma captura recebida, não é erro.
    if ((failure as NodeJS.ErrnoException).code !== "ENOENT") {
      error = "Não foi possível acessar a pasta entries. Verifique o caminho e as permissões.";
    }
  }

  const time = (value: string | null | undefined) => (value ? Date.parse(value) || 0 : 0);
  entries.sort((a, b) => time(b.timestamp) - time(a.timestamp) || a.id.localeCompare(b.id));

  // Missões conhecidas + missões que só aparecem nas capturas.
  const missions = new Map((await readMissions()).map((m) => [m.id, m]));
  for (const entry of entries) {
    if (!entry.missionId || missions.has(entry.missionId)) continue;
    const own = entries.filter((e) => e.missionId === entry.missionId).map((e) => e.timestamp).filter(Boolean) as string[];
    own.sort();
    missions.set(entry.missionId, { id: entry.missionId, started_at: own[0] ?? null, finished_at: own.at(-1) ?? null, synthetic: true });
  }
  const missionList = [...missions.values()].sort((a, b) => time(b.started_at) - time(a.started_at));

  const station = await readStation();
  const body = { station, missions: missionList, entries, error };
  const signature = createHash("sha1").update(JSON.stringify(body)).digest("hex").slice(0, 16);
  return { signature, ...body };
}
