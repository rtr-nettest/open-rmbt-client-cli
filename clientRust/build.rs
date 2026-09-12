fn main() {
    // Full `git describe` with commit hash, e.g. "v2.1-3-gc833667" — reported
    // as client_version.
    let full = git(&["describe", "--tags", "--always"]).unwrap_or_else(|| "dev".to_string());

    // Full describe suffixed with the current branch, e.g. "v2.1-3-gc833667-master"
    // — reported as software_revision, software_version and software_version_name.
    // If the branch is unknown, no suffix is added (rather than a placeholder).
    let branch = git(&["rev-parse", "--abbrev-ref", "HEAD"]).unwrap_or_default();
    let revision = if branch.is_empty() {
        full.clone()
    } else {
        format!("{full}-{branch}")
    };

    println!("cargo:rustc-env=GIT_VERSION_FULL={full}");
    println!("cargo:rustc-env=GIT_REVISION={revision}");
    println!("cargo:rerun-if-changed=.git/HEAD");
    println!("cargo:rerun-if-changed=.git/refs/tags");
}

/// Run `git <args>`, returning its trimmed stdout on success.
fn git(args: &[&str]) -> Option<String> {
    let out = std::process::Command::new("git").args(args).output().ok()?;
    out.status
        .success()
        .then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
        .filter(|s| !s.is_empty())
}
