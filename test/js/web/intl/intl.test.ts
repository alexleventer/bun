// ECMA-402 Intl coverage. Doubles as the regression net for the per-item zstd
// repack of ICU's display-name trees (curr/ lang/ region/ unit/ zone/): every
// non-en case in DisplayNames / NumberFormat(unit|currencyDisplay:"name") /
// DateTimeFormat(timeZoneName) reads a zstd-decompressed item, while Collator /
// Segmenter / default DateTimeFormat / default NumberFormat / normalize stay raw.
//
// Snapshots are the ground truth: they capture uncompressed-ICU output. If a
// decompressed item is wrong, the snapshot diff shows exactly which locale/tree.
// When WEBKIT_VERSION bumps ICU/CLDR, regenerate with `-u` against a build that
// links the unmodified libicudata.a.

import { CString, dlopen, ptr } from "bun:ffi";
import { describe, expect, test } from "bun:test";
import { bunEnv, bunExe, isLinux, isMacOS, isWindows, libcPathForDlopen } from "harness";
import { join } from "node:path";

// Snapshots are CLDR-version-specific. Only check them where Bun bundles the
// ICU they were generated against; macOS uses Apple's libicucore, so snapshot
// diffs there are expected and not a regression. The structural sweep below
// runs everywhere.
const SNAPSHOT_ICU_VERSION = "78.3";
const snapshotIf = !isMacOS && process.versions.icu === SNAPSHOT_ICU_VERSION ? test : test.skip;

const LOCALES = ["en", "de", "fr", "ja", "ko", "ru", "zh", "zh-Hant", "ar", "th", "es-419", "pt-PT"] as const;

// ---------------------------------------------------------------------------
// DisplayNames — region/ lang/ curr/ script (non-en compressed)
// ---------------------------------------------------------------------------

describe("Intl.DisplayNames", () => {
  for (const type of ["region", "language", "currency", "script"] as const) {
    const code = { region: "US", language: "en", currency: "USD", script: "Hant" }[type];
    snapshotIf(`${type}:'${code}' across locales`, () => {
      const out: Record<string, string | undefined> = {};
      for (const loc of LOCALES) out[loc] = new Intl.DisplayNames(loc, { type }).of(code);
      expect(out).toMatchSnapshot();
    });
  }

  snapshotIf("a few more region codes", () => {
    const out: Record<string, Record<string, string | undefined>> = {};
    for (const code of ["DE", "JP", "BR", "419"]) {
      out[code] = {};
      for (const loc of LOCALES) out[code][loc] = new Intl.DisplayNames(loc, { type: "region" }).of(code);
    }
    expect(out).toMatchSnapshot();
  });
});

// ---------------------------------------------------------------------------
// NumberFormat — default/currency-symbol raw; unit + currencyDisplay:"name" compressed
// ---------------------------------------------------------------------------

describe("Intl.NumberFormat", () => {
  snapshotIf("default grouping", () => {
    const out: Record<string, string> = {};
    for (const loc of LOCALES) out[loc] = new Intl.NumberFormat(loc).format(1234567.89);
    expect(out).toMatchSnapshot();
  });

  snapshotIf("currency symbol", () => {
    const out: Record<string, string> = {};
    for (const loc of LOCALES)
      out[loc] = new Intl.NumberFormat(loc, { style: "currency", currency: "EUR" }).format(1234.56);
    expect(out).toMatchSnapshot();
  });

  snapshotIf("currencyDisplay:'name' (curr/<loc>.res)", () => {
    const out: Record<string, string> = {};
    for (const loc of LOCALES)
      out[loc] = new Intl.NumberFormat(loc, { style: "currency", currency: "USD", currencyDisplay: "name" }).format(2);
    expect(out).toMatchSnapshot();
  });

  snapshotIf("style:'unit' (unit/<loc>.res; ru is the largest item)", () => {
    const out: Record<string, Record<string, string>> = {};
    for (const unit of ["kilometer", "celsius", "kilometer-per-hour"]) {
      out[unit] = {};
      for (const loc of LOCALES)
        out[unit][loc] = new Intl.NumberFormat(loc, { style: "unit", unit, unitDisplay: "long" }).format(5);
    }
    expect(out).toMatchSnapshot();
  });
});

// ---------------------------------------------------------------------------
// DateTimeFormat — default raw; timeZoneName (zone/<loc>.res) compressed
// ---------------------------------------------------------------------------

describe("Intl.DateTimeFormat", () => {
  snapshotIf("default", () => {
    const out: Record<string, string> = {};
    for (const loc of LOCALES) out[loc] = new Intl.DateTimeFormat(loc, { timeZone: "UTC" }).format(0);
    expect(out).toMatchSnapshot();
  });

  snapshotIf("timeZoneName:'long' (zone/<loc>.res)", () => {
    const tzName = (loc: string, tz: string) =>
      new Intl.DateTimeFormat(loc, { timeZone: tz, timeZoneName: "long" })
        .formatToParts(0)
        .find(p => p.type === "timeZoneName")!.value;
    const out: Record<string, Record<string, string>> = {};
    for (const tz of ["America/Los_Angeles", "Asia/Tokyo", "Europe/Berlin"]) {
      out[tz] = {};
      for (const loc of LOCALES) out[tz][loc] = tzName(loc, tz);
    }
    expect(out).toMatchSnapshot();
  });
});

// ---------------------------------------------------------------------------
// Time zone names with no '/' — aliases of city zones, with every tz data
// ---------------------------------------------------------------------------

// The tz database has EST5EDT, CST6CDT, MST7MDT and PST8PDT as zones of their own before 2024b and from 2026d on, and
// CET, EET, MET and WET before 2024b. ICU then holds the name canonical, and JavaScriptCore had no zone for it: as the
// zone of the process the name gave UTC, and as a time zone ID it was a RangeError. In the other tz releases each name
// is a link to a city zone. An alias of that city zone is what each is here, with every tz data (tc39/ecma402#1103).
describe.concurrent("time zone names with no '/'", () => {
  // getTimezoneOffset() for 09:00 local time on 15 January and on 15 July 2026, the city zone, and the name of its
  // standard time.
  const legacy: [name: string, january: number, july: number, city: string, standardTime: string][] = [
    ["EST5EDT", 300, 240, "America/New_York", "Eastern Standard Time"],
    ["CST6CDT", 360, 300, "America/Chicago", "Central Standard Time"],
    ["MST7MDT", 420, 360, "America/Denver", "Mountain Standard Time"],
    ["PST8PDT", 480, 420, "America/Los_Angeles", "Pacific Standard Time"],
    ["CET", -60, -120, "Europe/Brussels", "Central European Standard Time"],
    ["EET", -120, -180, "Europe/Athens", "Eastern European Standard Time"],
    ["MET", -60, -120, "Europe/Brussels", "Central European Standard Time"],
    ["WET", 0, -60, "Europe/Lisbon", "Western European Standard Time"],
  ];

  // probe() is what each way to name a zone gives for one of the names.
  const prelude = `
    import { setTimeZone } from "bun:jsc";
    const zone = () => [
      new Date(2026, 0, 15, 9).getTimezoneOffset(),
      new Date(2026, 6, 15, 9).getTimezoneOffset(),
      Intl.DateTimeFormat().resolvedOptions().timeZone,
    ];
    const attempt = fn => { try { return fn(); } catch (error) { return String(error); } };
    const january = new Date(Date.UTC(2026, 0, 15));
    const probe = ([name, , , city]) => ({
      assigned: (process.env.TZ = "Asia/Tokyo", process.env.TZ = name, zone()),
      option: attempt(() => new Intl.DateTimeFormat("en-US", { timeZone: name.toLowerCase() }).resolvedOptions().timeZone),
      standardTime: attempt(() => new Intl.DateTimeFormat("en-US", { timeZone: name, timeZoneName: "long" }).formatToParts(january).find(part => part.type === "timeZoneName").value),
      temporal: attempt(() => new Temporal.ZonedDateTime(0n, name).timeZoneId),
      isCity: attempt(() => new Temporal.ZonedDateTime(0n, name).equals(new Temporal.ZonedDateTime(0n, city))),
      cron: attempt(() => Bun.cron.parse("0 9 * * *", january, { tz: name }).toISOString()),
    });
    const listed = () => Intl.supportedValuesOf("timeZone").filter(name => !name.includes("/"));`;
  const expected = ([name, january, july, city, standardTime]: (typeof legacy)[number]) => ({
    assigned: [january, july, city],
    // An alias keeps its name.
    option: name,
    // ICU is asked for the city zone. For a zone of its own it has no name but "GMT-05:00".
    standardTime,
    temporal: name,
    isCity: true,
    cron: new Date(Date.UTC(2026, 0, 15, 9, january)).toISOString(),
  });

  async function run(env: Record<string, string>, script: string) {
    await using proc = Bun.spawn({
      cmd: [bunExe(), "-e", prelude + script],
      env: { ...bunEnv, ...env },
      stdout: "pipe",
      stderr: "pipe",
    });
    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
    return { result: JSON.parse(stdout || "null"), stderr, exitCode };
  }

  // The version of the tz data of the system ICU of macOS, and the zone IDs with no '/' that it holds canonical.
  function systemTimeZoneData() {
    const { symbols } = dlopen("/usr/lib/libicucore.A.dylib", {
      ucal_getTZDataVersion: { args: ["ptr"], returns: "ptr" },
      ucal_openTimeZoneIDEnumeration: { args: ["i32", "ptr", "ptr", "ptr"], returns: "ptr" },
      uenum_next: { args: ["ptr", "ptr", "ptr"], returns: "ptr" },
      uenum_close: { args: ["ptr"], returns: "void" },
    });
    const status = new Int32Array(1);
    const tz = new CString(symbols.ucal_getTZDataVersion(ptr(status))!).toString();
    const UCAL_ZONE_TYPE_CANONICAL = 1;
    const ids = symbols.ucal_openTimeZoneIDEnumeration(UCAL_ZONE_TYPE_CANONICAL, null, null, ptr(status));
    const canonical: string[] = [];
    for (let id; ids && (id = symbols.uenum_next(ids, null, ptr(status)));) {
      const name = new CString(id).toString();
      if (!name.includes("/")) canonical.push(name);
    }
    if (ids) symbols.uenum_close(ids);
    return { icu: process.versions.icu, tz, canonical };
  }

  // ICU reads the time zone files in ICU_TIMEZONE_FILES_DIR in place of its own. icu-tz-2024a/timezoneTypes.res is
  // tzdata/icunew/2024a/44/le/timezoneTypes.res of unicode-org/icu-data: with it ICU holds the four US names and
  // "Factory" canonical, whatever tz data it was built with. macOS has the ICU of the system, which has its own files.
  test.skipIf(isMacOS)("a name that ICU holds canonical is an alias of its city zone", async () => {
    const names = legacy.slice(0, 4);
    const { result, stderr, exitCode } = await run(
      { TZ: "EST5EDT", ICU_TIMEZONE_FILES_DIR: join(import.meta.dir, "icu-tz-2024a") },
      `
      console.log(JSON.stringify({
        start: [...zone(), new Date(2026, 0, 15, 9).toString()],
        names: ${JSON.stringify(names)}.map(probe),
        // setTimeZone() returns the name that ICU holds canonical: this shows that ICU read the file.
        setTimeZone: [attempt(() => setTimeZone("CST6CDT")), ...zone()],
        // With no zone set, the zone is the one of the host.
        host: (delete process.env.TZ, zone()),
        listed: listed(),
        // The tz database has no "SystemV/" zone, and its "Factory" zone is for a clock that is not set.
        notZones: ["Factory", "SystemV/EST5EDT"].map(name => attempt(() => new Intl.DateTimeFormat("en-US", { timeZone: name }).resolvedOptions().timeZone)),
      }));`,
    );
    expect(result).toEqual({
      start: [300, 240, "America/New_York", "Thu Jan 15 2026 09:00:00 GMT-0500 (Eastern Standard Time)"],
      names: names.map(expected),
      setTimeZone: ["CST6CDT", 360, 300, "America/Chicago"],
      // ICU takes the zone of the host from the TZ of the environment on Linux.
      host: isLinux ? [300, 240, "America/New_York"] : result?.host,
      listed: ["UTC"],
      notZones: ["RangeError: invalid time zone: Factory", "RangeError: invalid time zone: SystemV/EST5EDT"],
    });
    expect(stderr).toBe("");
    expect(exitCode).toBe(0);
  });

  // With the tz data that ICU has on this machine. The system ICU of macOS has the tz data of the machine: on a
  // macOS 15 machine of CI, EST5EDT is a zone of its own.
  test("each name is an alias of its city zone", async () => {
    // A run that passes says which tz data it went through.
    if (isMacOS) console.log("system ICU:", JSON.stringify(systemTimeZoneData()));
    const { result, stderr, exitCode } = await run(
      { TZ: "EST5EDT" },
      `
      console.log(JSON.stringify({
        start: zone(),
        names: ${JSON.stringify(legacy)}.map(probe),
        listed: listed(),
      }));`,
    );
    expect(result).toEqual({ start: [300, 240, "America/New_York"], names: legacy.map(expected), listed: ["UTC"] });
    expect(stderr).toBe("");
    expect(exitCode).toBe(0);
  });
});

// ---------------------------------------------------------------------------
// Collator — coll/* raw (incl. CJK tailorings)
// ---------------------------------------------------------------------------

describe("Intl.Collator", () => {
  snapshotIf("sort order across locales", () => {
    const out: Record<string, string[]> = {};
    for (const loc of LOCALES) out[loc] = ["z", "a", "ä", "ö", "Z", "A"].sort(new Intl.Collator(loc).compare);
    expect(out).toMatchSnapshot();
  });

  snapshotIf("zh pinyin (coll/zh.res, 713 KB raw)", () => {
    expect(["波", "次", "阿"].sort(new Intl.Collator("zh", { collation: "pinyin" }).compare)).toMatchSnapshot();
  });

  snapshotIf("ko", () => {
    expect(["하", "가", "나"].sort(new Intl.Collator("ko").compare)).toMatchSnapshot();
  });

  snapshotIf("de phonebook vs standard", () => {
    expect({
      standard: ["öf", "of"].sort(new Intl.Collator("de").compare),
      phonebook: ["öf", "of"].sort(new Intl.Collator("de-u-co-phonebk").compare),
    }).toMatchSnapshot();
  });

  test("sensitivity:'base' equates case and diacritics", () => {
    const c = new Intl.Collator("en", { sensitivity: "base" });
    expect([c.compare("a", "A"), c.compare("a", "á"), c.compare("a", "b"), c.compare("b", "a")]).toEqual([0, 0, -1, 1]);
  });

  // The default locale must not be ICU's en_US_POSIX fallback (what an invalid
  // platform language tag degrades to; bionic's default "C.UTF-8" used to
  // produce exactly that): its case-first collation gives "a".localeCompare("B") === 1.
  // Unix WTF maps the C locale to en-US. macOS and Windows report the UI
  // language of the machine, the same one this test process sees.
  // Concurrent: the children here run alongside the "locale variables" children below.
  test.concurrent("default locale is a real locale, not en-US-u-va-posix", async () => {
    await using proc = Bun.spawn({
      cmd: [
        bunExe(),
        "-e",
        `console.log(JSON.stringify([new Intl.Collator().resolvedOptions().locale, "a".localeCompare("B")]))`,
      ],
      // whatever the environment says, including nothing at all
      env: { ...bunEnv, LANG: undefined, LC_ALL: undefined, LC_CTYPE: undefined },
      stdout: "pipe",
      stderr: "pipe",
    });
    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
    const locale = isMacOS || isWindows ? new Intl.Collator().resolvedOptions().locale : "en-US";
    expect(stdout).toBe(JSON.stringify([locale, -1]) + "\n");
    expect(stderr).toBe("");
    expect(exitCode).toBe(0);
  });

  // Same path with the C locale spelled "C.UTF-8" (glibc/musl name for it, and
  // what bionic reports by default): WTF must still treat it as C -> "en-US".
  test.concurrent.skipIf(!isLinux)("default locale under C.UTF-8 is not en-US-u-va-posix", async () => {
    await using proc = Bun.spawn({
      cmd: [
        bunExe(),
        "-e",
        `const { dlopen } = require("bun:ffi");
         const libc = dlopen(${JSON.stringify(libcPathForDlopen())}, { setlocale: { args: ["i32", "cstring"], returns: "cstring" } });
         const LC_CTYPE = 0; // glibc, musl and bionic; the category platformLanguage() reads
         const set = libc.symbols.setlocale(LC_CTYPE, Buffer.from("C.UTF-8\\0"));
         console.log(JSON.stringify([set, new Intl.Collator().resolvedOptions().locale, "a".localeCompare("B"), (12345.5).toLocaleString()]));`,
      ],
      env: bunEnv,
      stdout: "pipe",
      stderr: "pipe",
    });
    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
    // setlocale returns null on a libc without a C.UTF-8 locale. The locale then
    // stays "C", which must give the same en-US output as the test above.
    const out = (set: string | null) => JSON.stringify([set, "en-US", -1, "12,345.5"]) + "\n";
    expect(stdout).toBeOneOf([out("C.UTF-8"), out(null)]);
    expect(stderr).toBe("");
    expect(exitCode).toBe(0);
  });
});

// ICU reads its own default locale from LC_ALL, then LC_MESSAGES, then LANG the
// first time a calendar or collator is opened. A value it cannot parse used to
// leave that default unset, and the first Date#toString / localeCompare / Intl
// constructor then crashed inside ICU. Bun's own default locale does not come
// from these variables, so every value, parseable or not, must give the en-US
// output. On Windows neither holds: ICU reads the system locale and JSC reports
// the UI language of the machine.
describe.skipIf(isWindows).concurrent("locale variables in the environment", () => {
  const script = `console.log(JSON.stringify([
    new Date(0).toString(),
    "a".localeCompare("b"),
    (1234.5).toLocaleString(),
    new Intl.DateTimeFormat().resolvedOptions().locale,
  ]))`;

  test.each([
    // eleven bytes is the longest language subtag ICU accepts
    { LANG: "abcdefghijkl" },
    { LANG: "/usr/lib/locale/en_US" },
    // ICU turns the modifier into a variant before it parses the value, and a
    // variant of 180 or more bytes is rejected. The value itself canonicalizes.
    { LANG: "en_US@k=" + Buffer.alloc(200, "a").toString() },
    { LC_MESSAGES: "abcdefghijkl" },
    { LC_ALL: "abcdefghijkl", LANG: "en_US.UTF-8" },
    // a parseable or empty variable in front of an unparseable one wins
    { LC_ALL: "C", LANG: "abcdefghijkl" },
    { LC_ALL: "", LANG: "abcdefghijkl" },
    { LC_ALL: "de_DE.UTF-8" },
  ])("%o", async vars => {
    await using proc = Bun.spawn({
      cmd: [bunExe(), "-e", script],
      env: { ...bunEnv, LANG: undefined, LC_ALL: undefined, LC_MESSAGES: undefined, ...vars },
      stdout: "pipe",
      stderr: "pipe",
    });
    const [stdout, stderr, exitCode] = await Promise.all([proc.stdout.text(), proc.stderr.text(), proc.exited]);
    expect(stdout).toBe(
      JSON.stringify(["Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)", -1, "1,234.5", "en-US"]) + "\n",
    );
    expect(stderr).toBe("");
    expect(exitCode).toBe(0);
  });
});

// ---------------------------------------------------------------------------
// Segmenter — brkitr/* raw (incl. cjdict)
// ---------------------------------------------------------------------------

describe("Intl.Segmenter", () => {
  const seg = (loc: string, g: Intl.SegmenterOptions["granularity"], s: string) =>
    [...new Intl.Segmenter(loc, { granularity: g }).segment(s)].map(x => x.segment);

  snapshotIf("grapheme — emoji ZWJ sequence", () => {
    expect(seg("en", "grapheme", "👨‍👩‍👧‍👦a🇯🇵")).toMatchSnapshot();
  });

  snapshotIf("word — en/zh/ja/th", () => {
    expect({
      en: seg("en", "word", "hello world"),
      zh: seg("zh", "word", "中文分词测试"),
      ja: seg("ja", "word", "今日はいい天気"),
      th: seg("th", "word", "สวัสดีครับ"),
    }).toMatchSnapshot();
  });
});

// ---------------------------------------------------------------------------
// PluralRules / ListFormat / RelativeTimeFormat — supplemental, raw
// ---------------------------------------------------------------------------

describe("Intl.PluralRules", () => {
  snapshotIf("select across locales", () => {
    const out: Record<string, Record<number, string>> = {};
    for (const loc of ["en", "ru", "ar", "pl"]) {
      out[loc] = {};
      for (const n of [0, 1, 2, 3, 5, 11, 21]) out[loc][n] = new Intl.PluralRules(loc).select(n);
    }
    expect(out).toMatchSnapshot();
  });
});

describe("Intl.ListFormat", () => {
  snapshotIf("conjunction across locales", () => {
    const out: Record<string, string> = {};
    for (const loc of LOCALES) out[loc] = new Intl.ListFormat(loc, { type: "conjunction" }).format(["a", "b", "c"]);
    expect(out).toMatchSnapshot();
  });
});

describe("Intl.RelativeTimeFormat", () => {
  snapshotIf("format across locales", () => {
    const out: Record<string, string[]> = {};
    for (const loc of LOCALES) {
      const f = new Intl.RelativeTimeFormat(loc);
      out[loc] = [f.format(-1, "day"), f.format(2, "day"), f.format(-3, "month")];
    }
    expect(out).toMatchSnapshot();
  });
});

// ---------------------------------------------------------------------------
// String / URL paths through ICU — raw
// ---------------------------------------------------------------------------

describe("String.prototype.normalize", () => {
  test("NFC/NFD round-trip", () => {
    const nfd = "café";
    expect(nfd.normalize("NFC")).toBe("café");
    expect("café".normalize("NFD")).toBe(nfd);
    expect(nfd.normalize("NFC").normalize("NFD")).toBe(nfd);
  });
});

describe("String.prototype.toLocale*Case", () => {
  test("tr dotted/dotless i", () => {
    expect("I".toLocaleLowerCase("tr")).toBe("ı");
    expect("i".toLocaleUpperCase("tr")).toBe("İ");
  });
});

describe("URL IDNA", () => {
  test("non-ASCII hostname → punycode", () => {
    expect(new URL("https://例え.jp").hostname).toBe("xn--r8jz45g.jp");
    expect(new URL("https://bücher.de").hostname).toBe("xn--bcher-kva.de");
  });
});

describe("Intl.getCanonicalLocales", () => {
  test("deprecated BCP-47 tags map to modern equivalents", () => {
    // ICU ships .res bundles under the deprecated tag names; canonicalization
    // is what makes them reachable.
    expect({
      in: Intl.getCanonicalLocales("in")[0],
      iw: Intl.getCanonicalLocales("iw")[0],
      mo: Intl.getCanonicalLocales("mo")[0],
      ji: Intl.getCanonicalLocales("ji")[0],
    }).toEqual({ in: "id", iw: "he", mo: "ro", ji: "yi" });
    // sh/tl/no are kept as-is (ICU ships bundles under both names)
    expect(Intl.getCanonicalLocales(["sh", "tl", "no"])).toEqual(["sh", "tl", "no"]);
  });
});

// ---------------------------------------------------------------------------
// Exhaustive sweep — load EVERY compressed item.
//
// icu-locales.txt is the full set of locales present in ICU's display-name
// trees (extracted from the package at build time). Iterating each × the five
// tree-touching APIs forces every region/ lang/ curr/ unit/ zone/ item through
// the decompress hook. A corrupt item surfaces as a throw or empty string;
// "everything fell back to root" surfaces as low distinct-value count.
//
// Regenerate the fixture when WEBKIT_VERSION bumps ICU:
//   icupkg -l icudt<NN>l.dat | grep -E '^(curr|lang|region|unit|zone)/' \
//     | sed -E 's|.*/||; s|\.res$||; s|_|-|g' | sort -u > icu-locales.txt
// ---------------------------------------------------------------------------

import { readFileSync } from "node:fs";

describe("exhaustive locale sweep (every compressed item)", () => {
  const all = readFileSync(new URL("./icu-locales.txt", import.meta.url), "utf8")
    .split("\n")
    .map(s => s.trim())
    .filter(Boolean)
    // ICU's tree includes legacy/alias tags (e.g. no_NO_NY) that aren't valid
    // BCP-47; getCanonicalLocales throws on those, so drop them up front.
    .filter(tag => {
      try {
        Intl.getCanonicalLocales(tag);
        return true;
      } catch {
        return false;
      }
    });

  const locales = Intl.DisplayNames.supportedLocalesOf(all);

  type Tree = "region" | "lang" | "curr" | "unit" | "zone";
  const probe: Record<Tree, (loc: string) => string | undefined> = {
    region: loc => new Intl.DisplayNames(loc, { type: "region" }).of("US"),
    lang: loc => new Intl.DisplayNames(loc, { type: "language" }).of("en"),
    curr: loc => new Intl.DisplayNames(loc, { type: "currency" }).of("USD"),
    unit: loc => new Intl.NumberFormat(loc, { style: "unit", unit: "meter", unitDisplay: "long" }).format(1),
    zone: loc =>
      new Intl.DateTimeFormat(loc, { timeZone: "America/Los_Angeles", timeZoneName: "long" })
        .formatToParts(0)
        .find(p => p.type === "timeZoneName")?.value,
  };

  for (const tree of Object.keys(probe) as Tree[]) {
    test(`${tree}/ — ${locales.length} locales, non-empty + locale-varying`, () => {
      const seen = new Set<string>();
      // One assertion per tree: two expect() calls per locale cost 0.6 s across
      // the five trees on a debug build, and a failure should name every broken
      // locale at once.
      const empty: string[] = [];
      for (const loc of locales) {
        const v = probe[tree](loc);
        if (typeof v !== "string" || v.length === 0) empty.push(loc);
        else seen.add(v);
      }
      expect(empty).toEqual([]);
      // Regional variants (en-GB, ar-AE, …) legitimately share strings with
      // their base locale, so the bar is "many distinct", not "all distinct".
      expect(seen.size).toBeGreaterThan(50);
    });
  }

  test("repeat calls return identical results (cache consistency)", () => {
    for (const loc of ["ko", "ru", "zh-Hant", "yo", "ar-EG"]) {
      const a = new Intl.DisplayNames(loc, { type: "region" }).of("US");
      const b = new Intl.DisplayNames(loc, { type: "region" }).of("US");
      expect(a).toBe(b);
    }
  });
});
