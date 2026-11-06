"use client";

import { fmt } from "../../lib/format";
import type { ModulePoll } from "../../lib/types";
import { Icon } from "./Icon";

// Faixa de acompanhamento da missão ao vivo, sobre o mapa.
export function MissionBanner({ poll, now }: { poll: ModulePoll | null; now: number }) {
  const status = poll?.status;
  const mission = status?.mission;
  if (!status || !mission || (status.state !== "in_mission" && status.state !== "uploading")) return null;

  const started = Date.parse(mission.started_at);
  const enabledAt = mission.captures_enabled_at ? Date.parse(mission.captures_enabled_at) : started + 120_000;
  const warmup = now < enabledAt;
  const warmupProgress = Math.min(1, Math.max(0, (now - started) / (enabledAt - started || 1)));

  if (status.state === "uploading") {
    const sent = status.upload?.sent ?? 0;
    const total = status.upload?.total ?? sent + (status.upload?.pending ?? 0);
    return (
      <div className="mission-banner phase-upload">
        <div className="banner-icon"><Icon name="upload" size={18} /></div>
        <div className="banner-body">
          <strong>Descarregando capturas</strong>
          <span>{sent} de {total} fotos recebidas pela estação</span>
          <div className="banner-progress"><i style={{ width: `${total ? (sent / total) * 100 : 0}%` }} /></div>
        </div>
      </div>
    );
  }

  const phase = !poll.connected ? "lost" : mission.ready_to_finish ? "ready" : warmup ? "warmup" : "capture";
  const lastCapture = mission.last_capture_at;

  return (
    <div className={`mission-banner phase-${phase}`}>
      <div className="banner-live"><span className="pulse-dot" /> AO VIVO</div>
      <div className="banner-stat">
        <span>Tempo de missão</span>
        <strong className="mono">{fmt.duration(now - started)}</strong>
      </div>
      <div className="banner-divider" />
      <div className="banner-stat">
        <span>Fotos</span>
        <strong className="mono">{mission.captures ?? 0}</strong>
      </div>
      {(mission.captures ?? 0) > 0 && (
        <>
          <div className="banner-divider" />
          <div className="banner-stat" title="Fotos já confirmadas pela estação durante o voo">
            <span>Na estação</span>
            <strong className="mono">
              {status.upload?.sent ?? 0}
              <small>/{status.upload?.total ?? mission.captures}</small>
            </strong>
            <div className="banner-progress mini">
              <i style={{ width: `${Math.min(100, ((status.upload?.sent ?? 0) / Math.max(1, status.upload?.total ?? mission.captures ?? 1)) * 100)}%` }} />
            </div>
          </div>
        </>
      )}
      <div className="banner-divider" />
      <div className="banner-phase">
        {phase === "warmup" && (
          <>
            <div className="ring" style={{ ["--p" as string]: warmupProgress }}><Icon name="clock" size={14} /></div>
            <span><strong>Estabilizando sensores</strong>Fotos liberadas em {fmt.duration(enabledAt - now)}</span>
          </>
        )}
        {phase === "capture" && (
          <>
            <div className="ring ring-spin"><Icon name="camera" size={14} /></div>
            <span><strong>Capturando</strong>{lastCapture ? `Última foto ${fmt.ago(lastCapture, now)}` : "Aguardando estabilidade"}</span>
          </>
        )}
        {phase === "ready" && (
          <>
            <div className="ring ring-ready"><Icon name="home" size={14} /></div>
            <span><strong>De volta à estação</strong>Pronto para finalizar a missão</span>
          </>
        )}
        {phase === "lost" && (
          <>
            <div className="ring ring-lost"><Icon name="wifiOff" size={14} /></div>
            <span><strong>Fora de alcance</strong>O módulo segue capturando; aguarde o retorno</span>
          </>
        )}
      </div>
    </div>
  );
}
