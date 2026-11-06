import { readStation, removeStation, writeStation } from "../../../lib/entries";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return Response.json({ station: await readStation() });
}

export async function PUT(request: Request) {
  const body = await request.json().catch(() => null);
  const latitude = Number(body?.latitude);
  const longitude = Number(body?.longitude);
  if (!Number.isFinite(latitude) || !Number.isFinite(longitude) || Math.abs(latitude) > 90 || Math.abs(longitude) > 180) {
    return Response.json({ error: "Coordenadas inválidas." }, { status: 400 });
  }
  const station = { latitude, longitude, name: typeof body?.name === "string" ? body.name.slice(0, 80) : null };
  await writeStation(station);
  return Response.json({ station });
}

// Remove a posição da estação; o mapa volta a pedir que ela seja marcada.
export async function DELETE() {
  await removeStation();
  return Response.json({ station: null });
}
