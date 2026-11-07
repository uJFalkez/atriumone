"use client";

import dynamic from "next/dynamic";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { fmt } from "../../lib/format";
import { clusterHotspots, coveredArea } from "../../lib/hotspots";
import { ndviGradientCss } from "../../lib/scale";
import type { DataPayload, EntrySummary, ModulePoll, ModuleStatus, Station } from "../../lib/types";
import { CalibrationDialog, ConfirmDialog, StationPanel, Toasts, type Toast } from "./Dialogs";
import { Icon } from "./Icon";
import { CaptureDetail, MissionOverview } from "./Inspector";
import type { HotspotRef, TrailPoint } from "./MapView";
import { MissionBanner } from "./MissionBanner";
import { deviceHealth } from "./ModulePanel";
import { Sidebar, type MissionView } from "./Sidebar";
import { TopBar, type Command } from "./TopBar";

const MapView = dynamic(() => import("./MapView").then((m) => m.MapView), {
  ssr: false,
  loading: () => <div className="map map-loading"><span className="spinner large" /></div>,
});

const DATA_POLL_MS = 3000;
const MODULE_POLL_MS = 2000;

function buildMissions(data: DataPayload, liveId: string | null): MissionView[] {
  const groups = new Map<string, EntrySummary[]>();
  for (const entry of data.entries) {
    const key = entry.missionId && data.missions.some((m) => m.id === entry.missionId) ? entry.missionId : "loose";
    groups.set(key, [...(groups.get(key) || []), entry]);
  }
  const chronological = [...data.missions].sort((a, b) => (Date.parse(a.started_at || "") || 0) - (Date.parse(b.started_at || "") || 0));
  const indexOf = new Map(chronological.map((m, i) => [m.id, i + 1]));

  const view = (id: string, entries: EntrySummary[], extra: Partial<MissionView>): MissionView => {
    const ordered = [...entries].sort((a, b) => (a.sequence ?? 0) - (b.sequence ?? 0) || (Date.parse(a.timestamp || "") || 0) - (Date.parse(b.timestamp || "") || 0));
    // Médias, focos e área só com as fotos do levantamento (fora: chão,
    // decolagem/pouso, RGB estourada). Todas continuam na lista de capturas.
    const survey = ordered.filter((e) => e.survey);
    const withNdvi = survey.filter((e) => e.ndvi);
    const clusters = clusterHotspots(survey);
    return {
      id,
      index: null,
      label: "",
      startedAt: null,
      finishedAt: null,
      live: false,
      entries: ordered,
      clusters,
      hotspots: clusters.length,
      severe: clusters.filter((c) => c.severity === "alta").length,
      meanNdvi: withNdvi.length ? withNdvi.reduce((s, e) => s + e.ndvi!.mean, 0) / withNdvi.length : null,
      areaM2: coveredArea(survey.flatMap((e) => (e.footprint ? [e.footprint] : []))),
      processing: ordered.filter((e) => e.status === "processing").length,
      track: null,
      registered: false,
      declaredCaptures: null,
      ...extra,
    };
  };

  const list = data.missions.map((m) =>
    view(m.id, groups.get(m.id) || [], {
      index: indexOf.get(m.id) ?? null,
      label: `Missão ${String(indexOf.get(m.id) ?? "").padStart(2, "0")}`,
      startedAt: m.started_at,
      finishedAt: m.finished_at ?? null,
      live: m.id === liveId,
      forced: m.forced,
      track: m.track ?? null,
      registered: !!m.registered,
      declaredCaptures: m.captures ?? null,
    }),
  );
  const loose = groups.get("loose");
  if (loose?.length) list.push(view("loose", loose, { label: "Capturas avulsas", startedAt: loose.at(-1)?.timestamp ?? null }));
  return list;
}

export default function Dashboard({ initial }: { initial: DataPayload }) {
  const [data, setData] = useState(initial);
  const [poll, setPoll] = useState<ModulePoll | null>(null);
  const lastStatus = useRef<ModuleStatus | null>(null);
  const [lastSeen, setLastSeen] = useState<number | null>(null);
  const [now, setNow] = useState(() => Date.now());

  const [missionId, setMissionId] = useState<string | null>(null);
  const [entryId, setEntryId] = useState<string | null>(null);
  const [hotspot, setHotspot] = useState<HotspotRef | null>(null);
  const [expanded, setExpanded] = useState(false); // visualizador ampliado (sobrevive à troca de foto)
  const [focusKey, setFocusKey] = useState(0);

  const [showHotspots, setShowHotspots] = useState(true);
  const [showTrack, setShowTrack] = useState(true);

  const [busy, setBusy] = useState<Command | null>(null);
  const [dialog, setDialog] = useState<null | "start" | "force" | "calibration" | "removeStation">(null);
  const [toasts, setToasts] = useState<Toast[]>([]);
  const [placing, setPlacing] = useState(false);
  const [pendingStation, setPendingStation] = useState<[number, number] | null>(null);
  const [track, setTrack] = useState<{ missionId: string; samples: number; points: [number, number, number, number][] } | null>(null);
  const [trail, setTrail] = useState<{ missionId: string | null; points: TrailPoint[] }>({ missionId: null, points: [] });

  const toast = useCallback((t: Omit<Toast, "id">) => {
    const id = Date.now() + Math.random();
    setToasts((list) => [...list, { ...t, id }]);
    setTimeout(() => setToasts((list) => list.filter((x) => x.id !== id)), t.tone === "error" ? 8000 : 4500);
  }, []);

  // ===== Polling =====
  const refreshData = useCallback(async () => {
    try {
      const response = await fetch(`/api/data?sig=${data.signature}`, { cache: "no-store" });
      const body = await response.json();
      if (!body.unchanged) setData(body);
    } catch {}
  }, [data.signature]);

  const refreshModule = useCallback(async () => {
    try {
      const response = await fetch("/api/module/status", { cache: "no-store" });
      const body: ModulePoll = await response.json();
      if (body.status) lastStatus.current = body.status;
      if (body.connected) setLastSeen(Date.now());
      setPoll(body);
      // Rastro ao vivo (só em memória): as fotos só chegam no fim da missão.
      const live = body.status?.live;
      const id = body.status?.state === "in_mission" ? body.status.mission?.id ?? null : null;
      if (id && live?.north_m != null && live.east_m != null) {
        const point: TrailPoint = [live.north_m, live.east_m];
        setTrail((t) => {
          const points = t.missionId === id ? t.points : [];
          const last = points.at(-1);
          if (last && Math.hypot(last[0] - point[0], last[1] - point[1]) < 0.5) return t.missionId === id ? t : { missionId: id, points };
          return { missionId: id, points: [...points, point].slice(-2000) };
        });
      }
    } catch {
      // Falha da própria estação: segue tentando no próximo ciclo.
      setPoll({ connected: false, latencyMs: null, checkedAt: new Date().toISOString(), status: null });
    }
  }, []);

  useEffect(() => {
    const timer = setInterval(refreshData, DATA_POLL_MS);
    return () => clearInterval(timer);
  }, [refreshData]);

  useEffect(() => {
    let stop = false;
    let timer: ReturnType<typeof setTimeout>;
    const loop = async () => {
      await refreshModule();
      if (!stop) timer = setTimeout(loop, MODULE_POLL_MS);
    };
    loop();
    return () => { stop = true; clearTimeout(timer); };
  }, [refreshModule]);

  useEffect(() => {
    const timer = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(timer);
  }, []);

  // Sem conexão, mantém o último estado conhecido (ex.: missão em andamento
  // fora do alcance) para a interface não "esquecer" a missão.
  const effectivePoll = useMemo<ModulePoll | null>(
    () => (poll && !poll.connected && lastStatus.current ? { ...poll, status: lastStatus.current } : poll),
    [poll],
  );
  const liveStatus = effectivePoll?.status;
  const liveMissionId = liveStatus && (liveStatus.state === "in_mission" || liveStatus.state === "uploading") ? liveStatus.mission?.id ?? null : null;

  // ===== Derivados =====
  const missions = useMemo(() => buildMissions(data, liveMissionId), [data, liveMissionId]);
  const mission = missions.find((m) => m.id === missionId) || missions.find((m) => m.live) || missions[0] || null;
  const entry = mission?.entries.find((e) => e.id === entryId) || null;
  const entryIndex = entry && mission ? mission.entries.indexOf(entry) : -1;

  // Histórico de posições da missão selecionada (registrado pelo módulo).
  const trackSamples = mission?.track?.samples ?? 0;
  useEffect(() => {
    if (!mission || !trackSamples) return;
    if (track?.missionId === mission.id && track.samples === trackSamples) return;
    let cancelled = false;
    fetch(`/api/missions/${mission.id}/track`)
      .then((r) => (r.ok ? r.json() : null))
      .then((body) => !cancelled && body && setTrack({ missionId: mission.id, samples: trackSamples, points: body.points }))
      .catch(() => undefined);
    return () => { cancelled = true; };
  }, [mission?.id, trackSamples, track?.missionId, track?.samples]);

  // Uma missão nova iniciada aqui passa a ser a selecionada.
  const previousLive = useRef<string | null>(null);
  useEffect(() => {
    if (liveMissionId && liveMissionId !== previousLive.current) {
      setMissionId(liveMissionId);
      setEntryId(null);
      setHotspot(null);
    }
    previousLive.current = liveMissionId;
  }, [liveMissionId]);

  // ===== Comandos =====
  const send = useCallback(async (command: Command) => {
    const force = command === "mission/finish?force";
    const action = force ? "mission/finish" : command;
    setBusy(command);
    try {
      const response = await fetch(`/api/module/${action}${force ? "?force=true" : ""}`, { method: "POST" });
      const body = await response.json().catch(() => ({}));
      if (!response.ok) throw new Error(body.error || "O módulo recusou o comando.");
      if (action === "calibrate") setDialog("calibration");
      if (action === "mission/start") toast({ tone: "success", title: "Missão iniciada", text: "As fotos serão liberadas após 2 minutos de estabilização." });
      if (action === "mission/finish") toast({ tone: "success", title: "Missão finalizada", text: "O módulo está enviando as capturas para a estação." });
      await Promise.all([refreshModule(), refreshData()]);
    } catch (error) {
      toast({ tone: "error", title: "Comando não executado", text: error instanceof Error ? error.message : undefined });
    } finally {
      setBusy(null);
    }
  }, [refreshData, refreshModule, toast]);

  const onCommand = (command: Command) => {
    if (command === "mission/start") setDialog("start");
    else if (command === "mission/finish?force") setDialog("force");
    else send(command);
  };

  // ===== Estação =====
  const saveStation = async ([longitude, latitude]: [number, number]) => {
    const response = await fetch("/api/station", { method: "PUT", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ latitude, longitude }) });
    if (!response.ok) return toast({ tone: "error", title: "Coordenadas inválidas" });
    const { station } = (await response.json()) as { station: Station };
    setData((d) => ({ ...d, station }));
    setPlacing(false);
    setPendingStation(null);
    toast({ tone: "success", title: "Estação posicionada" });
  };

  const removeStation = async () => {
    setDialog(null);
    const response = await fetch("/api/station", { method: "DELETE" });
    if (!response.ok) return toast({ tone: "error", title: "Não foi possível remover a estação" });
    setData((d) => ({ ...d, station: null }));
    setPlacing(false);
    setPendingStation(null);
    toast({ tone: "success", title: "Estação removida", text: "Marque a nova posição no mapa para posicionar as fotos." });
  };

  // ===== Seleção =====
  const selectMission = (id: string) => {
    setMissionId(id);
    setEntryId(null);
    setHotspot(null);
  };
  const selectEntry = (id: string | null, focus = false) => {
    setEntryId(id);
    setHotspot(null);
    if (focus && id) setFocusKey((k) => k + 1);
  };
  const selectHotspot = (ref: HotspotRef, focus = false) => {
    setEntryId(ref.entryId);
    setHotspot(ref);
    if (focus) setFocusKey((k) => k + 1);
  };
  const step = useCallback((delta: number) => {
    if (!mission || entryIndex < 0) return;
    const next = mission.entries[entryIndex + delta];
    if (next) {
      setEntryId(next.id);
      setHotspot(null);
      setFocusKey((k) => k + 1);
    }
  }, [mission, entryIndex]);

  const live = liveStatus?.live && effectivePoll?.connected && liveStatus.live.north_m != null && liveStatus.live.east_m != null
    ? { north_m: liveStatus.live.north_m, east_m: liveStatus.live.east_m, yaw_deg: liveStatus.live.yaw_deg ?? null }
    : null;

  return (
    <div className="shell">
      <TopBar poll={effectivePoll} lastSeen={lastSeen} now={now} busy={busy} onCommand={onCommand} />

      <div className="body">
        <Sidebar
          missions={missions}
          selectedMission={mission}
          selectedEntry={entry?.id ?? null}
          onSelectMission={selectMission}
          onSelectEntry={(id) => selectEntry(id, true)}
          now={now}
        />

        <main className="stage">
          <MapView
            station={data.station}
            entries={mission?.entries.filter((e) => e.survey) ?? []}
            clusters={mission?.clusters ?? []}
            selectedEntryId={entry?.id ?? null}
            selectedHotspot={hotspot}
            showHotspots={showHotspots}
            showTrack={showTrack}
            live={live}
            track={track && track.missionId === mission?.id ? track.points : null}
            trail={trail.missionId && trail.missionId === mission?.id && trail.missionId === liveMissionId ? trail.points : []}
            fitKey={mission?.id ?? "none"}
            focusKey={focusKey}
            placingStation={placing}
            onPlaceStation={setPendingStation}
            onSelectEntry={(id) => selectEntry(id)}
            onSelectHotspot={(ref) => selectHotspot(ref)}
          />

          <div className="map-top">
            <MissionBanner poll={effectivePoll} now={now} />
            <StationPanel
              station={data.station}
              placing={placing}
              pending={pendingStation}
              onStartPlacing={() => setPlacing(true)}
              onCancel={() => { setPlacing(false); setPendingStation(null); }}
              onSave={saveStation}
              onRemove={() => setDialog("removeStation")}
            />
          </div>

          <div className="map-controls">
            <div className="control-group">
              <button className={`toggle ${showHotspots ? "on" : ""}`} aria-pressed={showHotspots} onClick={() => setShowHotspots((v) => !v)}><Icon name="target" size={14} />Focos</button>
              <button className={`toggle ${showTrack ? "on" : ""}`} aria-pressed={showTrack} onClick={() => setShowTrack((v) => !v)}><Icon name="route" size={14} />Trajeto</button>
              {data.station && !placing && (
                <button className="toggle" onClick={() => setPlacing(true)} title="Reposicionar a estação"><Icon name="home" size={14} />Estação</button>
              )}
            </div>
          </div>

          {showTrack && (mission?.track?.imu_fraction ?? 0) > 0 && (
            <div className="map-imu-note" role="note">
              <i aria-hidden="true" />
              Trecho tracejado: posição só pela IMU (sem GPS), pouco confiável · {fmt.percent(mission!.track!.imu_fraction)} do trajeto
            </div>
          )}

          {(mission?.entries.length ?? 0) > 0 && (
            <div className="map-legend">
              <span className="control-label">NDVI médio da foto</span>
              <div className="legend-gradient" style={{ background: ndviGradientCss }} />
              <div className="legend-ticks"><span>≤ −0,2</span><span>+0,2</span><span>≥ +0,6</span></div>
              <div className="legend-caption"><span>solo, água, estresse</span><span>vegetação densa</span></div>
            </div>
          )}

          {data.error && <div className="map-error"><Icon name="alert" size={16} />{data.error}</div>}
        </main>

        <aside className="inspector">
          {entry && mission ? (
            <CaptureDetail
              key={entry.id}
              entry={entry}
              position={entryIndex}
              total={mission.entries.length}
              selectedHotspot={hotspot?.entryId === entry.id ? hotspot.id : null}
              onSelectHotspot={(id) => selectHotspot({ entryId: entry.id, id }, true)}
              onStep={step}
              expanded={expanded}
              onExpandedChange={setExpanded}
              onClose={() => { setExpanded(false); selectEntry(null); }}
            />
          ) : mission ? (
            <MissionOverview
              mission={mission}
              selectedEntry={null}
              onSelectEntry={(id) => selectEntry(id, true)}
              onSelectHotspot={(entryId, id) => selectHotspot({ entryId, id }, true)}
              now={now}
            />
          ) : (
            <div className="inspector-empty">
              <div className="empty-art"><Icon name="satellite" size={34} stroke={1.3} /></div>
              <h2>Pronto para a primeira missão</h2>
              <p>Calibre os sensores, posicione o módulo na estação e clique em <strong>Iniciar missão</strong>. As fotos aparecem aqui quando o módulo voltar.</p>
            </div>
          )}
        </aside>
      </div>

      {dialog === "start" && (
        <ConfirmDialog
          icon="play"
          title="Iniciar missão?"
          confirm="Iniciar missão"
          onClose={() => setDialog(null)}
          onConfirm={() => { setDialog(null); send("mission/start"); }}
        >
          <p>O módulo vai <strong>zerar a posição e a altura aqui</strong>. Ele precisa estar parado na estação.</p>
          <ul className="checklist">
            <li><Icon name="check" size={14} />Fotos liberadas após 2 min de estabilização</li>
            <li><Icon name="check" size={14} />Uma foto a cada 10 s, quando estável</li>
            <li><Icon name="check" size={14} />Finalização liberada ao voltar à estação</li>
          </ul>
          {liveStatus?.sensors && !liveStatus.sensors.calibrated && (
            <div className="notice"><Icon name="alert" size={14} />Sensores sem calibração. Considere calibrar antes.</div>
          )}
          {(liveStatus?.devices ?? []).filter((d) => deviceHealth(d) !== "ok").map((d) => (
            <div key={d.id} className="notice"><Icon name="alert" size={14} />{d.label || d.id.toUpperCase()}: {!d.connected ? "desconectado" : d.message || "com aviso"}</div>
          ))}
        </ConfirmDialog>
      )}
      {dialog === "force" && (
        <ConfirmDialog
          icon="alert"
          tone="danger"
          title="Forçar finalização?"
          confirm="Finalizar mesmo assim"
          onClose={() => setDialog(null)}
          onConfirm={() => { setDialog(null); send("mission/finish?force"); }}
        >
          <p>O módulo ainda não reportou o retorno à estação. A missão será encerrada agora e as capturas, enviadas.</p>
        </ConfirmDialog>
      )}
      {dialog === "removeStation" && (
        <ConfirmDialog
          icon="home"
          tone="danger"
          title="Remover a estação?"
          confirm="Remover"
          onClose={() => setDialog(null)}
          onConfirm={removeStation}
        >
          <p>As fotos e trajetos deixam de aparecer no mapa até você marcar uma nova posição. Nenhum dado das missões é apagado.</p>
        </ConfirmDialog>
      )}
      {dialog === "calibration" && <CalibrationDialog poll={effectivePoll} onClose={() => setDialog(null)} />}

      <Toasts toasts={toasts} onDismiss={(id) => setToasts((list) => list.filter((t) => t.id !== id))} />
      <span className="sr-only" aria-live="polite">{entry ? `Foto ${entry.sequence ?? ""} selecionada, ${fmt.time(entry.timestamp)}` : ""}</span>
    </div>
  );
}
