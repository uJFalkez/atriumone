"use client";

import type { GeoJSONSource, ImageSource, Map as MapLibreMap, MapLayerMouseEvent, Marker, Popup } from "maplibre-gl";
import { useEffect, useRef, useState } from "react";
import { fmt } from "../../lib/format";
import { toLngLat } from "../../lib/geo";
import type { HotspotCluster } from "../../lib/hotspots";
import { ndviMapExpression, SEVERITY } from "../../lib/scale";
import type { EntrySummary, Station } from "../../lib/types";

export type ImageLayer = "ndvi" | "rgb" | "none";
export type HotspotRef = { entryId: string; id: number };
type LiveModule = { north_m: number; east_m: number; yaw_deg: number | null } | null;
export type TrailPoint = [number, number]; // [north_m, east_m]

const DEFAULT_CENTER: [number, number] = [-46.7296, -23.5558]; // Escola Politécnica (USP)

const SATELLITE_STYLE = {
  version: 8 as const,
  sources: {
    satellite: {
      type: "raster" as const,
      tiles: ["https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"],
      tileSize: 256,
      maxzoom: 19,
      attribution: "Imagens © Esri, Maxar, Earthstar Geographics",
    },
  },
  layers: [
    { id: "background", type: "background" as const, paint: { "background-color": "#0a110f" } },
    {
      id: "satellite",
      type: "raster" as const,
      source: "satellite",
      paint: { "raster-saturation": -0.25, "raster-brightness-max": 0.82, "raster-contrast": 0.08 },
    },
  ],
};

const empty = { type: "FeatureCollection" as const, features: [] as GeoJSON.Feature[] };
const IMU_COLOR = "#f2b44c"; // posição só por IMU: pouco confiável

function escapeHtml(text: string) {
  return text.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!);
}

export function MapView({
  station,
  entries,
  clusters,
  selectedEntryId,
  selectedHotspot,
  imageLayer,
  showHotspots,
  showTrack,
  live,
  trail,
  track,
  fitKey,
  focusKey,
  placingStation,
  onPlaceStation,
  onSelectEntry,
  onSelectHotspot,
}: {
  station: Station | null;
  entries: EntrySummary[];
  clusters: HotspotCluster[];
  selectedEntryId: string | null;
  selectedHotspot: HotspotRef | null;
  imageLayer: ImageLayer;
  showHotspots: boolean;
  showTrack: boolean;
  live: LiveModule;
  trail: TrailPoint[];
  track: [number, number, number, number][] | null; // histórico de posições: [n, e, t_s, confiável 1 | só IMU 0]
  fitKey: string;
  focusKey: number;
  placingStation: boolean;
  onPlaceStation: (lngLat: [number, number]) => void;
  onSelectEntry: (id: string) => void;
  onSelectHotspot: (ref: HotspotRef) => void;
}) {
  const container = useRef<HTMLDivElement>(null);
  const mapRef = useRef<MapLibreMap | null>(null);
  const libRef = useRef<typeof import("maplibre-gl") | null>(null);
  const stationMarker = useRef<Marker | null>(null);
  const liveMarker = useRef<Marker | null>(null);
  const popup = useRef<Popup | null>(null);
  const images = useRef(new Map<string, string>());
  const hasHotspots = useRef(false);
  const hoveredId = useRef<string | null>(null);
  const refreshHover = useRef<() => void>(() => undefined);
  const handlers = useRef({ onSelectEntry, onSelectHotspot, onPlaceStation, placingStation });
  handlers.current = { onSelectEntry, onSelectHotspot, onPlaceStation, placingStation };
  const [ready, setReady] = useState(false);

  // ===== Inicialização =====
  useEffect(() => {
    let disposed = false;
    let frame = 0;
    (async () => {
      const maplibre = await import("maplibre-gl");
      if (disposed || !container.current) return;
      libRef.current = maplibre;
      const map = new maplibre.Map({
        container: container.current,
        style: SATELLITE_STYLE,
        center: station ? [station.longitude, station.latitude] : DEFAULT_CENTER,
        zoom: station ? 17 : 15,
        maxZoom: 22,
        attributionControl: { compact: true },
        fadeDuration: 150,
      });
      mapRef.current = map;
      map.addControl(new maplibre.NavigationControl({ visualizePitch: true, showCompass: true }), "top-right");
      map.addControl(new maplibre.ScaleControl({ unit: "metric", maxWidth: 110 }), "top-right");
      map.on("error", (event) => console.error("[mapa]", event.error?.message || event.error));
      // Exposto para depuração e testes automatizados (ex.: Playwright).
      (window as unknown as { __atriumMap?: MapLibreMap }).__atriumMap = map;
      popup.current = new maplibre.Popup({ closeButton: false, closeOnClick: false, offset: 14, className: "map-popup", maxWidth: "260px" });

      map.on("load", () => {
        map.addSource("footprints", { type: "geojson", data: empty });
        map.addSource("track", { type: "geojson", data: empty });
        map.addSource("captures", { type: "geojson", data: empty });
        map.addSource("hotspots", { type: "geojson", data: empty });
        map.addSource("trail", { type: "geojson", data: empty });

        map.addLayer({ id: "footprint-fill", type: "fill", source: "footprints", paint: { "fill-color": "#ffffff", "fill-opacity": ["case", ["get", "selected"], 0.06, 0.0] } });
        map.addLayer({
          id: "footprint-outline", type: "line", source: "footprints",
          paint: {
            "line-color": ["case", ["get", "selected"], "#9cf0c6", ["get", "imu"], IMU_COLOR, "#ffffff"],
            "line-opacity": ["case", ["get", "selected"], 1, 0.35],
            "line-width": ["case", ["get", "selected"], 2.5, 1],
          },
        });
        map.addLayer({ id: "trail", type: "line", source: "trail", layout: { "line-cap": "round", "line-join": "round" }, paint: { "line-color": "#7cc7ff", "line-width": 2.5, "line-opacity": 0.85, "line-blur": 0.5 } });
        map.addLayer({ id: "track-glow", type: "line", source: "track", filter: ["==", ["get", "reliable"], true], layout: { "line-cap": "round", "line-join": "round" }, paint: { "line-color": "#9cf0c6", "line-width": ["case", ["get", "recorded"], 10, 8], "line-opacity": 0.16, "line-blur": 5 } });
        map.addLayer({
          id: "track", type: "line", source: "track", filter: ["==", ["get", "reliable"], true],
          layout: { "line-cap": "round", "line-join": "round" },
          paint: { "line-width": 2.5, "line-color": "#9cf0c6", "line-opacity": 0.95 },
        });
        // Trechos com posição só por IMU (sem fix GPS): tracejado âmbar.
        map.addLayer({
          id: "track-imu", type: "line", source: "track", filter: ["==", ["get", "reliable"], false],
          layout: { "line-cap": "butt", "line-join": "round" },
          paint: { "line-width": 2.5, "line-color": IMU_COLOR, "line-opacity": 0.9, "line-dasharray": [2, 1.6] },
        });
        map.addLayer({
          id: "hotspot-halo", type: "circle", source: "hotspots",
          paint: {
            "circle-color": "transparent",
            "circle-radius": 10,
            "circle-stroke-width": 2,
            "circle-stroke-color": ["match", ["get", "severity"], "alta", SEVERITY.alta.color, "media", SEVERITY.media.color, SEVERITY.baixa.color],
            "circle-stroke-opacity": 0.6,
          },
        });
        map.addLayer({
          id: "hotspot", type: "circle", source: "hotspots",
          paint: {
            "circle-color": ["match", ["get", "severity"], "alta", SEVERITY.alta.color, "media", SEVERITY.media.color, SEVERITY.baixa.color],
            "circle-radius": ["case", ["get", "selected"], 7, 4.5],
            "circle-stroke-color": "#0a110f",
            "circle-stroke-width": ["case", ["get", "selected"], 2.5, 1.5],
          },
        });
        map.addLayer({
          id: "captures", type: "circle", source: "captures",
          paint: {
            "circle-color": ["case", ["has", "ndvi"], ndviMapExpression(["get", "ndvi"]) as never, "#8a958f"],
            "circle-radius": ["case", ["get", "selected"], 8, ["get", "hovered"], 7, 4.5],
            "circle-stroke-color": ["case", ["get", "selected"], "#9cf0c6", ["get", "imu"], IMU_COLOR, "#ffffff"],
            "circle-stroke-width": ["case", ["get", "selected"], 3, 1.8],
          },
        });

        const enter = () => (map.getCanvas().style.cursor = handlers.current.placingStation ? "crosshair" : "pointer");
        const leave = () => {
          map.getCanvas().style.cursor = handlers.current.placingStation ? "crosshair" : "";
          popup.current?.remove();
        };
        for (const layer of ["captures", "hotspot", "footprint-fill"]) {
          map.on("mouseenter", layer, enter);
          map.on("mouseleave", layer, leave);
        }
        // Hover numa foto: mostra a própria foto. O conteúdo só é trocado
        // quando muda o ponto, para a imagem não piscar ao mover o mouse.
        let hovered: string | null = null;
        map.on("mousemove", "captures", (event: MapLayerMouseEvent) => {
          const feature = event.features?.[0];
          const p = feature?.properties;
          if (!p || p.id === hovered) return;
          hovered = p.id;
          hoveredId.current = p.id;
          const coordinates = (feature!.geometry as GeoJSON.Point).coordinates as [number, number];
          const image = p.thumb
            ? `<img class="popup-photo" src="${escapeHtml(p.thumb)}" alt="" onload="this.classList.add('ready')" />`
            : `<div class="popup-photo pending">${p.status === "error" ? "Falha no processamento" : "Processando na estação…"}</div>`;
          popup.current!
            .setLngLat(coordinates)
            .setHTML(
              `<div class="popup-capture">${image}<div class="popup-body">` +
              `<strong>Foto ${escapeHtml(String(p.sequence ?? "—"))}<span>${escapeHtml(p.time)}</span></strong>` +
              `<dl><dt>NDVI médio</dt><dd>${escapeHtml(p.ndviText)}</dd><dt>Focos</dt><dd>${p.hotspots}</dd><dt>Altura</dt><dd>${escapeHtml(p.height)}</dd></dl>` +
              (p.imu ? `<p class="popup-warn">Posição só pela IMU (sem GPS)${p.sigma ? ` · ±${escapeHtml(p.sigma)}` : ""}: localização incerta.</p>` : "") +
              `</div></div>`,
            )
            .addTo(map);
          refreshHover.current();
        });
        map.on("mouseleave", "captures", () => {
          hovered = null;
          hoveredId.current = null;
          refreshHover.current();
        });
        map.on("mousemove", "hotspot", (event: MapLayerMouseEvent) => {
          const p = event.features?.[0]?.properties;
          if (!p) return;
          const severity = SEVERITY[p.severity as keyof typeof SEVERITY];
          popup.current!.setLngLat(event.lngLat).setHTML(
            `<strong><i class="sev-dot" style="background:${severity.color}"></i>Foco · severidade ${severity.label.toLowerCase()}</strong>` +
            `<dl><dt>Queda de NDVI</dt><dd>${escapeHtml(p.drop)}</dd><dt>Área</dt><dd>${escapeHtml(p.area)}</dd><dt>Visto nas fotos</dt><dd>${escapeHtml(String(p.sequence ?? "—"))}</dd></dl>`,
          ).addTo(map);
        });

        map.on("click", (event) => {
          if (handlers.current.placingStation) {
            handlers.current.onPlaceStation([event.lngLat.lng, event.lngLat.lat]);
            return;
          }
          const hit = map.queryRenderedFeatures(event.point, { layers: ["hotspot", "captures", "footprint-fill"] });
          const hotspot = hit.find((f) => f.layer.id === "hotspot");
          if (hotspot) return handlers.current.onSelectHotspot({ entryId: hotspot.properties.entryId, id: hotspot.properties.hid });
          const capture = hit.find((f) => f.layer.id === "captures") || hit.find((f) => f.layer.id === "footprint-fill");
          if (capture) handlers.current.onSelectEntry(capture.properties.id);
        });

        // Pulso dos focos: ~24 fps e só quando há focos visíveis, para não
        // manter o mapa redesenhando sem necessidade.
        let last = 0;
        const animate = (t: number) => {
          frame = requestAnimationFrame(animate);
          if (t - last < 42 || !hasHotspots.current || document.hidden) return;
          last = t;
          const phase = (t % 1800) / 1800;
          map.setPaintProperty("hotspot-halo", "circle-radius", 5 + phase * 16);
          map.setPaintProperty("hotspot-halo", "circle-stroke-opacity", 0.75 * (1 - phase));
        };
        frame = requestAnimationFrame(animate);
        setReady(true);
      });
    })();
    return () => {
      disposed = true;
      cancelAnimationFrame(frame);
      mapRef.current?.remove();
      mapRef.current = null;
      images.current.clear();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ===== Cursor no modo de posicionar a estação =====
  useEffect(() => {
    const map = mapRef.current;
    if (map) map.getCanvas().style.cursor = placingStation ? "crosshair" : "";
  }, [placingStation, ready]);

  // ===== Marcador da estação =====
  useEffect(() => {
    const map = mapRef.current;
    const lib = libRef.current;
    if (!map || !lib || !ready) return;
    stationMarker.current?.remove();
    stationMarker.current = null;
    if (!station) return;
    const element = document.createElement("div");
    element.className = "station-marker";
    element.innerHTML = `<span class="station-ring"></span><span class="station-core"><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M4 11 12 4l8 7v9h-5v-6H9v6H4v-9Z"/></svg></span><span class="station-label">Estação</span>`;
    stationMarker.current = new lib.Marker({ element }).setLngLat([station.longitude, station.latitude]).addTo(map);
  }, [station, ready]);

  // ===== Módulo ao vivo =====
  useEffect(() => {
    const map = mapRef.current;
    const lib = libRef.current;
    if (!map || !lib || !ready) return;
    if (!live || !station) {
      liveMarker.current?.remove();
      liveMarker.current = null;
      return;
    }
    if (!liveMarker.current) {
      const element = document.createElement("div");
      element.className = "live-marker";
      element.innerHTML = `<span class="live-pulse"></span><span class="live-arrow"><svg width="22" height="22" viewBox="0 0 24 24"><path d="M12 3 19 20 12 16 5 20Z" fill="currentColor"/></svg></span>`;
      liveMarker.current = new lib.Marker({ element, rotationAlignment: "map" }).setLngLat([0, 0]).addTo(map);
    }
    liveMarker.current.setLngLat(toLngLat(station, live.north_m, live.east_m)).setRotation(live.yaw_deg ?? 0);
  }, [live, station, ready]);

  // ===== Realce do ponto sob o mouse =====
  useEffect(() => {
    refreshHover.current = () => {
      const map = mapRef.current;
      if (!map?.getLayer("captures")) return;
      const id = hoveredId.current ?? "";
      map.setPaintProperty("captures", "circle-radius", ["case", ["get", "selected"], 8, ["==", ["get", "id"], id], 7, 4.5]);
    };
  }, []);

  // ===== Rastro do módulo durante a missão =====
  useEffect(() => {
    const map = mapRef.current;
    if (!map || !ready) return;
    const coordinates = station ? trail.map(([n, e]) => toLngLat(station, n, e)) : [];
    (map.getSource("trail") as GeoJSONSource).setData(
      coordinates.length > 1 ? { type: "Feature", geometry: { type: "LineString", coordinates }, properties: {} } : empty,
    );
  }, [trail, station, ready]);

  // ===== Dados da missão =====
  useEffect(() => {
    const map = mapRef.current;
    if (!map || !ready) return;

    const placed = station ? entries.filter((e) => e.position?.north_m != null && e.position?.east_m != null) : [];
    const ll = (n: number, e: number) => toLngLat(station!, n, e);

    (map.getSource("captures") as GeoJSONSource).setData({
      type: "FeatureCollection",
      features: placed.map((e) => ({
        type: "Feature",
        geometry: { type: "Point", coordinates: ll(e.position!.north_m!, e.position!.east_m!) },
        properties: {
          id: e.id,
          sequence: e.sequence,
          selected: e.id === selectedEntryId,
          hotspots: e.hotspots.length,
          time: fmt.time(e.timestamp),
          height: e.position?.height_m != null ? `${fmt.number(e.position.height_m, 1)} m` : "—",
          hovered: false,
          imu: e.position?.source === "imu",
          sigma: e.position?.sigma_m != null ? `${fmt.number(e.position.sigma_m, 0)} m` : "",
          status: e.status,
          thumb: e.status === "processed" ? `/api/entries/${e.id}/preview/thumb.jpg?v=${e.version}` : "",
          ndviText: e.ndvi ? fmt.signed(e.ndvi.mean) : e.status === "processing" ? "processando…" : "—",
          ...(e.ndvi ? { ndvi: e.ndvi.mean } : {}),
        },
      })),
    });

    const withFootprint = station ? entries.filter((e) => e.footprint) : [];
    (map.getSource("footprints") as GeoJSONSource).setData({
      type: "FeatureCollection",
      features: withFootprint.map((e) => {
        const ring = e.footprint!.map(([n, east]) => ll(n, east));
        return { type: "Feature", geometry: { type: "Polygon", coordinates: [[...ring, ring[0]]] }, properties: { id: e.id, selected: e.id === selectedEntryId, imu: e.position?.source === "imu" } };
      }),
    });

    // Trajeto: histórico registrado (quebrado em trechos GPS / só IMU) ou,
    // sem histórico, a ligação dos pontos das fotos.
    const runs: { reliable: boolean; coordinates: [number, number][] }[] = [];
    if (station && track && track.length > 1) {
      for (const [n, e, , reliable] of track) {
        const ok = reliable !== 0;
        const point = ll(n, e);
        const last = runs.at(-1);
        if (last && last.reliable === ok) last.coordinates.push(point);
        else runs.push({ reliable: ok, coordinates: last ? [last.coordinates.at(-1)!, point] : [point] });
      }
    } else if (station) {
      const ordered = [...placed].sort((a, b) => (a.sequence ?? 0) - (b.sequence ?? 0));
      runs.push({ reliable: true, coordinates: [[0, 0], ...ordered.map((e) => [e.position!.north_m!, e.position!.east_m!])].map(([n, e]) => ll(n, e)) });
    }
    (map.getSource("track") as GeoJSONSource).setData({
      type: "FeatureCollection",
      features: showTrack
        ? runs.filter((r) => r.coordinates.length > 1).map((r) => ({
            type: "Feature" as const,
            geometry: { type: "LineString" as const, coordinates: r.coordinates },
            properties: { reliable: r.reliable, recorded: !!track },
          }))
        : [],
    });

    hasHotspots.current = showHotspots && clusters.length > 0;
    (map.getSource("hotspots") as GeoJSONSource).setData({
      type: "FeatureCollection",
      features: showHotspots && station
        ? clusters
            .filter((c) => c.north_m != null && c.east_m != null)
            .map((c) => ({
              type: "Feature" as const,
              geometry: { type: "Point" as const, coordinates: ll(c.north_m!, c.east_m!) },
              properties: {
                entryId: c.best.entryId, hid: c.best.id, severity: c.severity,
                sequence: c.refs.map((r) => r.sequence ?? "—").join(", "),
                selected: !!selectedHotspot && c.refs.some((r) => r.entryId === selectedHotspot.entryId && r.id === selectedHotspot.id),
                drop: fmt.number(c.ndvi_drop, 2), area: c.area_m2 != null ? `${fmt.number(c.area_m2, 1)} m²` : "—",
              },
            }))
        : [],
    });

    // Imagens projetadas no solo (pegada de cada foto)
    const wanted = new Map<string, { url: string; coordinates: [number, number][] }>();
    if (imageLayer !== "none") {
      for (const e of withFootprint) {
        if (e.status !== "processed") continue;
        wanted.set(e.id, {
          url: `/api/entries/${e.id}/preview/${imageLayer === "ndvi" ? "ndvi.png" : "rgb.jpg"}?v=${e.version}`,
          coordinates: e.footprint!.map(([n, east]) => ll(n, east)),
        });
      }
    }
    for (const id of [...images.current.keys()]) {
      if (!wanted.has(id)) {
        if (map.getLayer(`img-${id}`)) map.removeLayer(`img-${id}`);
        if (map.getSource(`img-${id}`)) map.removeSource(`img-${id}`);
        images.current.delete(id);
      }
    }
    for (const [id, { url, coordinates }] of wanted) {
      const key = `${url}|${coordinates.flat().join(",")}`;
      const coords = coordinates as [[number, number], [number, number], [number, number], [number, number]];
      if (!images.current.has(id)) {
        map.addSource(`img-${id}`, { type: "image", url, coordinates: coords });
        map.addLayer({ id: `img-${id}`, type: "raster", source: `img-${id}`, paint: { "raster-opacity": 0.92, "raster-fade-duration": 200 } }, "footprint-fill");
      } else if (images.current.get(id) !== key) {
        (map.getSource(`img-${id}`) as ImageSource).updateImage({ url, coordinates: coords });
      }
      images.current.set(id, key);
    }
  }, [entries, clusters, track, station, selectedEntryId, selectedHotspot, imageLayer, showHotspots, showTrack, ready]);

  // ===== Enquadramento ao trocar de missão =====
  useEffect(() => {
    const map = mapRef.current;
    const lib = libRef.current;
    if (!map || !lib || !ready || !station) return;
    const points: [number, number][] = [[station.longitude, station.latitude]];
    for (const [n, east] of track ?? []) points.push(toLngLat(station, n, east));
    for (const e of entries) {
      if (e.footprint) points.push(...e.footprint.map(([n, east]) => toLngLat(station, n, east)));
      else if (e.position?.north_m != null && e.position?.east_m != null) points.push(toLngLat(station, e.position.north_m, e.position.east_m));
    }
    if (points.length === 1) {
      map.easeTo({ center: points[0], zoom: Math.max(map.getZoom(), 17), duration: 900 });
      return;
    }
    const bounds = points.reduce((b, p) => b.extend(p), new lib.LngLatBounds(points[0], points[0]));
    map.fitBounds(bounds, { padding: { top: 110, bottom: 70, left: 70, right: 70 }, maxZoom: 20, duration: 1100 });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [fitKey, ready, !!station, !!track]);

  // ===== Foco em uma foto ou ponto selecionado na lista =====
  useEffect(() => {
    const map = mapRef.current;
    if (!map || !ready || !station || !focusKey) return;
    let target: [number, number] | null = null;
    if (selectedHotspot) {
      const c = clusters.find((x) => x.refs.some((r) => r.entryId === selectedHotspot.entryId && r.id === selectedHotspot.id));
      if (c?.north_m != null && c.east_m != null) target = toLngLat(station, c.north_m, c.east_m);
    } else if (selectedEntryId) {
      const e = entries.find((x) => x.id === selectedEntryId);
      if (e?.position?.north_m != null && e.position.east_m != null) target = toLngLat(station, e.position.north_m, e.position.east_m);
    }
    if (target) map.flyTo({ center: target, zoom: Math.max(map.getZoom(), selectedHotspot ? 20 : 18.5), duration: 900, essential: true });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focusKey]);

  return <div ref={container} className="map" aria-label="Mapa da missão" />;
}
