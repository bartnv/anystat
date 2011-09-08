<?php

$logdir = "/var/stats/";

header("Content-Type: text/plain");

$dh = opendir($logdir);

if ($_GET['source']) {
  while (($file = readdir($dh)) != false) {
    $pattern = "/^" . $_GET['source'] . "\.(.*?)\.?([0-9]+)\.log$/";
    if (preg_match($pattern, $file, $matches)) $stats[$matches[1]][] = $matches[2];
  }
}
else {
  while (($file = readdir($dh)) != false) {
    $pattern = "/^([^.]+)\.(.*?)\.?[0-9]+\.log$/";
    if (preg_match($pattern, $file, $matches)) $stats[$matches[1]][] = $matches[2];
  }
  $n = 1;
  foreach ($stats as $key => $value) {
    if ($n++ == 1) print("{\n  \"$key\" : [");
    else print(",\n  \"$key\" : [");
    $value = array_unique($value);
    sort($value);
    $nn = 1;
    foreach ($value as $val) {
      if ($nn++ == 1) {
        if ($val) print(" $val");
        else print(" total");
      }
      else {
        if ($val) print(", $val");
        else print(", total");
      }
    }
    print(" ]");
  }
  print("\n}");
  exit();
}

ksort($stats);

print("{\n  \"name\" : \"" . $_GET['source']. "\",\n");
print("  \"start\" : " . $_GET['start'] . ",\n");
print("  \"end\" : " . $_GET['end'] . ",\n");

$n = 1;
print("  \"keys\" : [");

foreach (array_keys($stats) as $key) {
  if ($key == "") $key = "total";
  if ($n++ == 1) print(" [ \"$key\" ]");
  else print(", [ \"$key\" ]");
}
print(" ],");

$n = 1;
print("\n  \"data\" : [\n");

foreach ($stats as $key => $versions) {
  sort($versions);
  if (!end($versions)) continue;
  while (prev($versions) > $_GET['start']);
  if (!current($versions)) reset($versions);

  if ($n++ == 1) print("    [");
  else print(",\n    [");

  $nn = 1;

  $deltalast = 0;
  do {
    if ($key == "") $filename = sprintf("%s.%d.log", $_GET['source'], current($versions));
    else $filename = sprintf("%s.%s.%d.log", $_GET['source'], $key, current($versions));
    $fh = fopen($logdir . $filename, "r");
    while ($line = fgets($fh)) {
      $items = explode(",", rtrim($line));
      if ($items[0] < $_GET['start']) continue;
      if ($items[0] > $_GET['end']) break;
      if ($nn++ == 1) print(" [ " . ($items[0] - $_GET['start']) . ", " . rtrim(rtrim($items[1], '0'), '.') . " ]");
      else print(", [ " . ($items[0] - $_GET['start']) . ", " . rtrim(rtrim($items[1], '0'), '.') . " ]");
      if ($deltalast) {
        $deltasum += $items[0] - $deltalast;
        $deltan++;
      }
      $deltalast = $items[0];
    }
    if ($line) break;
  } while (next($versions));

  print(" ]");
}

print("\n  ]\n, \"delta\" : " . $deltasum/$deltan . " }");
