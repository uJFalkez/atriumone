// Instrumentos de atitude: horizonte artificial (roll/pitch) e bússola (yaw).

import { fmt } from "../../lib/format";

export function AttitudeIndicator({ roll, pitch }: { roll: number | null; pitch: number | null }) {
  const r = roll ?? 0;
  const p = Math.max(-40, Math.min(40, pitch ?? 0));
  const shift = p * 1.1; // px por grau
  const available = roll !== null && pitch !== null;
  return (
    <figure className={`instrument ${available ? "" : "unavailable"}`}>
      <svg viewBox="-60 -60 120 120" role="img" aria-label={`Roll ${fmt.number(roll, 1)} graus, pitch ${fmt.number(pitch, 1)} graus`}>
        <defs>
          <clipPath id="horizon-clip"><circle r="52" /></clipPath>
        </defs>
        <g clipPath="url(#horizon-clip)">
          <g transform={`rotate(${-r}) translate(0 ${shift})`}>
            <rect x="-120" y="-160" width="240" height="160" className="sky" />
            <rect x="-120" y="0" width="240" height="160" className="ground" />
            <line x1="-120" x2="120" y1="0" y2="0" className="horizon" />
            {[-20, -10, 10, 20].map((d) => (
              <line key={d} x1={-(Math.abs(d) === 10 ? 10 : 16)} x2={Math.abs(d) === 10 ? 10 : 16} y1={-d * 1.1} y2={-d * 1.1} className="pitch-line" />
            ))}
          </g>
        </g>
        <circle r="52" className="bezel" />
        <path d="M-26 0 H-9 L0 7 L9 0 H26" className="reticle" />
        <circle r="2" className="reticle-dot" />
        <g transform={`rotate(${-r})`}><path d="M0 -51 l-4 7 h8 Z" className="roll-pointer" /></g>
        <path d="M0 -58 l-3 -0.1 l3 5 l3 -5 Z" className="roll-ref" />
      </svg>
      <figcaption>
        <span>Roll <strong className="mono">{fmt.number(roll, 1)}°</strong></span>
        <span>Pitch <strong className="mono">{fmt.number(pitch, 1)}°</strong></span>
      </figcaption>
    </figure>
  );
}

export function Compass({ heading }: { heading: number | null }) {
  const h = heading ?? 0;
  const labels: [number, string][] = [[0, "N"], [90, "L"], [180, "S"], [270, "O"]];
  return (
    <figure className={`instrument ${heading === null ? "unavailable" : ""}`}>
      <svg viewBox="-60 -60 120 120" role="img" aria-label={`Rumo ${fmt.number(heading, 0)} graus`}>
        <circle r="52" className="dial" />
        <g transform={`rotate(${-h})`}>
          {Array.from({ length: 36 }, (_, i) => i * 10).map((d) => (
            <line key={d} x1="0" x2="0" y1={-52} y2={d % 90 === 0 ? -43 : d % 30 === 0 ? -46 : -49} transform={`rotate(${d})`} className={d % 90 === 0 ? "tick-major" : "tick-minor"} />
          ))}
          {labels.map(([d, l]) => (
            <text key={l} transform={`rotate(${d}) translate(0 -32) rotate(${-d + h})`} className={`cardinal ${l === "N" ? "north" : ""}`} textAnchor="middle" dominantBaseline="central">{l}</text>
          ))}
        </g>
        <path d="M0 -24 L7 10 L0 5 L-7 10 Z" className="heading-arrow" />
        <path d="M0 -58 l-4 -0.1 l4 6 l4 -6 Z" className="roll-ref" />
      </svg>
      <figcaption>
        <span>Rumo <strong className="mono">{fmt.number(heading, 0)}°</strong></span>
      </figcaption>
    </figure>
  );
}
