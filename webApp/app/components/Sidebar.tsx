"use client";

import { fmt } from "../../lib/format";
import { ndviColor } from "../../lib/scale";
import type { HotspotCluster } from "../../lib/hotspots";
import type { EntrySummary, TrackStats } from "../../lib/types";
import { Icon } from "./Icon";

export type MissionView = {
  id: string; // id da missão ou "loose"
  index: number | null;
  label: string;
  startedAt: string | null;
  finishedAt: string | null;
  live: boolean;
  forced?: boolean;
  entries: EntrySummary[]; // em ordem de captura
  clusters: HotspotCluster[]; // focos únicos (detecções repetidas agrupadas)
  track: TrackStats | null; // histórico de posições enviado pelo módulo
  registered: boolean;
  declaredCaptures: number | null;
  hotspots: number;
  severe: number;
  meanNdvi: number | null;
  areaM2: number;
  processing: number;
};

export function thumbUrl(entry: EntrySummary) {
  if (entry.status === "processed") return `/api/entries/${entry.id}/preview/thumb.jpg?v=${entry.version}`;
  return null;
}

function MissionCard({ mission, selected, onSelect, now }: { mission: MissionView; selected: boolean; onSelect: () => void; now: number }) {
  const duration = mission.startedAt
    ? (mission.finishedAt ? Date.parse(mission.finishedAt) : mission.live ? now : null) ?? null
    : null;
  return (
    <button className={`mission-card ${selected ? "selected" : ""} ${mission.live ? "live" : ""}`} onClick={onSelect} aria-current={selected || undefined}>
      <div className="mission-card-top">
        <span className="mission-index">{mission.index !== null ? String(mission.index).padStart(2, "0") : "··"}</span>
        <div className="mission-card-title">
          <strong>{mission.label}</strong>
          <span>
            {mission.startedAt ? `${fmt.date(mission.startedAt, false)} · ${fmt.time(mission.startedAt, false)}` : "Sem missão associada"}
            {duration !== null && mission.startedAt ? ` · ${fmt.duration(duration - Date.parse(mission.startedAt))}` : ""}
          </span>
        </div>
        {mission.live && <span className="live-badge"><span className="pulse-dot" />Ao vivo</span>}
      </div>
      <div className="mission-card-stats">
        <span title="Fotos"><Icon name="camera" size={13} />{mission.entries.length}</span>
        <span title="Focos de NDVI baixo (únicos)" className={mission.hotspots ? "has-hotspots" : ""}><Icon name="target" size={13} />{mission.hotspots}</span>
        <span title="Área coberta"><Icon name="scan" size={13} />{fmt.area(mission.areaM2 || null)}</span>
        {mission.meanNdvi !== null && (
          <span className="ndvi-chip" title="NDVI médio">
            <i style={{ background: ndviColor(mission.meanNdvi) }} />
            {fmt.signed(mission.meanNdvi)}
          </span>
        )}
      </div>
    </button>
  );
}

export function Sidebar({
  missions,
  selectedMission,
  selectedEntry,
  onSelectMission,
  onSelectEntry,
  now,
}: {
  missions: MissionView[];
  selectedMission: MissionView | null;
  selectedEntry: string | null;
  onSelectMission: (id: string) => void;
  onSelectEntry: (id: string | null) => void;
  now: number;
}) {
  return (
    <aside className="sidebar" aria-label="Missões">
      <div className="sidebar-section-title">
        <span>Missões</span>
        <span className="count">{missions.filter((m) => m.id !== "loose").length}</span>
      </div>
      <div className="mission-list">
        {missions.map((mission) => (
          <MissionCard key={mission.id} mission={mission} selected={selectedMission?.id === mission.id} onSelect={() => onSelectMission(mission.id)} now={now} />
        ))}
        {!missions.length && (
          <div className="sidebar-empty">
            <Icon name="route" size={26} />
            <strong>Nenhuma missão ainda</strong>
            <span>Conecte o módulo e clique em <em>Iniciar missão</em>.</span>
          </div>
        )}
      </div>

      {selectedMission && (
        <>
          <div className="sidebar-section-title">
            <span>Capturas</span>
            <span className="count">{selectedMission.entries.length}</span>
          </div>
          <div className="capture-list">
            {[...selectedMission.entries].reverse().map((entry) => {
              const thumb = thumbUrl(entry);
              const severe = entry.hotspots.some((h) => h.severity === "alta");
              return (
                <button key={entry.id} className={`capture-row ${selectedEntry === entry.id ? "selected" : ""}`} onClick={() => onSelectEntry(selectedEntry === entry.id ? null : entry.id)}>
                  <span className="capture-thumb">
                    {thumb ? <img src={thumb} alt="" loading="lazy" /> : <span className={`thumb-state ${entry.status}`}>{entry.status === "error" ? <Icon name="alert" size={16} /> : <span className="spinner" />}</span>}
                    {entry.ndvi && <i className="thumb-ndvi" style={{ background: ndviColor(entry.ndvi.mean) }} />}
                  </span>
                  <span className="capture-info">
                    <strong>{entry.sequence != null ? `Foto ${entry.sequence}` : "Captura"}</strong>
                    <span className="mono">{fmt.time(entry.timestamp)}</span>
                  </span>
                  <span className="capture-meta">
                    {entry.status === "processing" && <span className="tag">processando</span>}
                    {entry.status === "error" && <span className="tag tag-error">erro</span>}
                    {entry.status === "processed" && !entry.survey && <span className="tag" title={entry.flags.join(", ")}>fora</span>}
                    {entry.hotspots.length > 0 && (
                      <span className={`hotspot-count ${severe ? "severe" : ""}`}><Icon name="target" size={12} />{entry.hotspots.length}</span>
                    )}
                  </span>
                </button>
              );
            })}
            {!selectedMission.entries.length && (
              <p className="sidebar-note">{selectedMission.live ? "As fotos aparecem aqui sempre que o módulo alcança a estação, inclusive durante o voo." : "Nenhuma captura recebida."}</p>
            )}
          </div>
        </>
      )}
    </aside>
  );
}
