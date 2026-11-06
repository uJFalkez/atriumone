import { readMissions, upsertMission } from "./missions";
import type { ModulePoll, ModuleStatus } from "./types";

export const moduleUrl = (process.env.MODULE_URL || "http://pi3b:8000").replace(/\/$/, "");

// Cada tentativa tem um limite para o laço de polling não travar; uma
// tentativa sem resposta não é erro, só "ainda não encontrado".
const STATUS_TIMEOUT_MS = 2500;
const COMMAND_TIMEOUT_MS = 8000;

export async function moduleRequest(pathname: string, init: RequestInit = {}, timeout = COMMAND_TIMEOUT_MS) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  try {
    return await fetch(`${moduleUrl}${pathname}`, { ...init, signal: controller.signal, cache: "no-store" });
  } finally {
    clearTimeout(timer);
  }
}

export async function pollModule(): Promise<ModulePoll> {
  const started = performance.now();
  const checkedAt = new Date().toISOString();
  try {
    const response = await moduleRequest("/status", {}, STATUS_TIMEOUT_MS);
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const status = (await response.json()) as ModuleStatus;
    await syncMissions(status).catch(() => undefined);
    return { connected: true, latencyMs: Math.round(performance.now() - started), checkedAt, status };
  } catch (error) {
    // Registrado só para depuração; a interface trata como "procurando".
    const reason = error instanceof Error ? (error.name === "AbortError" ? "sem resposta" : error.message) : "sem resposta";
    return { connected: false, latencyMs: null, checkedAt, status: null, error: reason };
  }
}

// Mantém o registro local de missões coerente com o que o módulo reporta,
// mesmo que a missão tenha sido iniciada ou finalizada fora do dashboard.
async function syncMissions(status: ModuleStatus) {
  const current = status.mission;
  if (current?.id) {
    await upsertMission(current.id, { started_at: current.started_at, module_id: status.module_id ?? null });
  }
  if (status.state === "idle" || status.state === "calibrating") {
    const open = (await readMissions()).filter((m) => !m.finished_at && m.id !== current?.id);
    await Promise.all(open.map((m) => upsertMission(m.id, { finished_at: new Date().toISOString() })));
  }
}

export async function moduleError(response: Response) {
  try {
    const body = await response.json();
    if (typeof body?.detail === "string") return body.detail;
    if (typeof body?.error === "string") return body.error;
  } catch {}
  return `O módulo respondeu HTTP ${response.status}.`;
}
