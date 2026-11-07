const paths = {
  play: "M7 4.5v15l12.5-7.5L7 4.5Z",
  flag: "M5 21V4m0 0h11l-2 4 2 4H5",
  compass: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Zm3.5 5.5-2 5-5 2 2-5 5-2Z",
  orbit: "M12 12m-2 0a2 2 0 1 0 4 0 2 2 0 1 0-4 0M4.5 8.5C3 5.5 3.7 3.7 6 3.7c3 0 7.5 3.4 10.8 8.3 3.3 4.9 3.7 8.3.8 8.3-2 0-4.6-1.5-7.1-3.8M19.5 8.5C21 5.5 20.3 3.7 18 3.7c-3 0-7.5 3.4-10.8 8.3-3.3 4.9-3.7 8.3-.8 8.3 2 0 4.6-1.5 7.1-3.8",
  wifi: "M2 8.8a15 15 0 0 1 20 0M5 12.2a10.5 10.5 0 0 1 14 0M8.5 15.6a5.5 5.5 0 0 1 7 0M12 19h.01",
  wifiOff: "M2 2l20 20M8.5 15.6a5.5 5.5 0 0 1 7 0M12 19h.01M5 12.2a10.5 10.5 0 0 1 4.2-2.4M16.7 10.6A10.5 10.5 0 0 1 19 12.2M2 8.8a15 15 0 0 1 4.3-2.8M10.7 5.1A15 15 0 0 1 22 8.8",
  layers: "m12 3 9 5-9 5-9-5 9-5Zm-9 9 9 5 9-5M3 16l9 5 9-5",
  image: "M4 4h16v16H4zM4 16l5-5 4 4 3-3 4 4M8.5 8.5h.01",
  camera: "M3 8.5A1.5 1.5 0 0 1 4.5 7h2.6l1.6-2.4h6.6L16.9 7h2.6A1.5 1.5 0 0 1 21 8.5v9a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 17.5v-9ZM12 10a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Z",
  alert: "M12 3 2 20h20L12 3Zm0 6v5m0 3h.01",
  pin: "M19 10c0 5-7 11-7 11S5 15 5 10a7 7 0 1 1 14 0ZM12 7.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5Z",
  home: "M4 11 12 4l8 7v9h-5v-6H9v6H4v-9Z",
  upload: "M12 16V4m0 0-5 5m5-5 5 5M4 20h16",
  check: "M5 12.5 10 17l9-10",
  x: "M6 6l12 12M18 6 6 18",
  left: "M15 5l-7 7 7 7",
  right: "M9 5l7 7-7 7",
  clock: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Zm0 4.5V12l3 2",
  height: "M12 3v18M8 7l4-4 4 4M8 17l4 4 4-4",
  leaf: "M5 19c0-9 5-14 15-14 0 10-5 15-14 15M5 19c3-4 6-7 10-9",
  download: "M12 4v12m0 0-5-5m5 5 5-5M4 20h16",
  crosshair: "M12 3v4m0 10v4M3 12h4m10 0h4M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8Z",
  target: "M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18Zm0 4a5 5 0 1 0 0 10 5 5 0 0 0 0-10Zm0 4a1 1 0 1 0 0 2 1 1 0 0 0 0-2Z",
  more: "M5 12h.01M12 12h.01M19 12h.01",
  route: "M6 19a2 2 0 1 0 0-4 2 2 0 0 0 0 4Zm12-10a2 2 0 1 0 0-4 2 2 0 0 0 0 4ZM8 17h6.5a3.5 3.5 0 0 0 0-7h-5a3.5 3.5 0 0 1 0-7H16",
  scan: "M4 8V5a1 1 0 0 1 1-1h3M16 4h3a1 1 0 0 1 1 1v3M20 16v3a1 1 0 0 1-1 1h-3M8 20H5a1 1 0 0 1-1-1v-3M7 12h10",
  gauge: "M12 14l4-5M4.9 18a9 9 0 1 1 14.2 0",
  satellite: "M13 7 9 3 3 9l4 4m6-6 4 4m-4-4-6 6m10-2 4 4-6 6-4-4m6-6-6 6m-3 2a4 4 0 0 1-4-4m8 4a8 8 0 0 1-8-8",
  sparkle: "M12 3l1.8 5.2L19 10l-5.2 1.8L12 17l-1.8-5.2L5 10l5.2-1.8L12 3Z",
  expand: "M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5M4 4l5 5M20 4l-5 5M20 20l-5-5M4 20l5-5",
  external: "M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5",
} as const;

export type IconName = keyof typeof paths;

export function Icon({ name, size = 18, stroke = 1.7, className }: { name: IconName; size?: number; stroke?: number; className?: string }) {
  const filled = name === "play";
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill={filled ? "currentColor" : "none"}
      stroke="currentColor"
      strokeWidth={stroke}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      className={className}
    >
      <path d={paths[name]} />
    </svg>
  );
}
