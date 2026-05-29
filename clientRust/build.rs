fn main() {
    let out = std::process::Command::new("git")
        .args(["describe", "--tags", "--long", "--always"])
        .output();

    let version = match out {
        Ok(o) if o.status.success() => {
            let raw = String::from_utf8_lossy(&o.stdout);
            let raw = raw.trim();
            // "v1.0-5-gabcdef" → strip "-gabcdef" → "v1.0-5"
            // No tags: "abcdef1" → kept as-is
            raw.rfind('-')
               .filter(|&i| raw[i + 1..].starts_with('g'))
               .map(|i| raw[..i].to_string())
               .unwrap_or_else(|| raw.to_string())
        }
        _ => "dev".to_string(),
    };

    println!("cargo:rustc-env=GIT_VERSION={version}");
    println!("cargo:rerun-if-changed=.git/HEAD");
    println!("cargo:rerun-if-changed=.git/refs/tags");
}
