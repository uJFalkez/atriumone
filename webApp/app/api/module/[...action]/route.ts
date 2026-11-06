import { upsertMission } from "../../../../lib/missions";
import { moduleError, moduleRequest } from "../../../../lib/module";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const actions = new Set(["mission/start", "mission/finish", "calibrate"]);

export async function POST(request: Request, { params }: { params: Promise<{ action: string[] }> }) {
  const action = (await params).action.join("/");
  if (!actions.has(action)) return Response.json({ error: "Comando desconhecido." }, { status: 404 });

  const force = action === "mission/finish" && new URL(request.url).searchParams.get("force") === "true";
  let response: Response;
  try {
    response = await moduleRequest(`/${action}${force ? "?force=true" : ""}`, { method: "POST" });
  } catch {
    return Response.json({ error: "Módulo inacessível. Verifique a conexão." }, { status: 502 });
  }
  if (!response.ok) return Response.json({ error: await moduleError(response) }, { status: response.status });

  const body = await response.json().catch(() => ({}));
  const mission = body?.mission;
  if (mission?.id && action === "mission/start") {
    await upsertMission(mission.id, { started_at: mission.started_at ?? new Date().toISOString(), finished_at: null });
  }
  if (mission?.id && action === "mission/finish") {
    await upsertMission(mission.id, { finished_at: mission.finished_at ?? new Date().toISOString(), forced: force || undefined });
  }
  return Response.json(body);
}
