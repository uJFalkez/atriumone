import { readTrack, simplifyTrack } from "../../../../../lib/missions";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

// Histórico de posições da missão, reduzido para desenho: [north_m, east_m, t_s][]
export async function GET(_request: Request, { params }: { params: Promise<{ id: string }> }) {
  const track = await readTrack((await params).id);
  if (!track) return Response.json({ error: "Missão sem histórico de posições." }, { status: 404 });
  return Response.json({ points: simplifyTrack(track.points), stats: track.stats });
}
