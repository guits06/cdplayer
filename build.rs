fn main() {
    println!("cargo:rerun-if-changed=cdda_engine.c");
    println!("cargo:rerun-if-changed=cdda_engine.h");

    cc::Build::new()
        .file("cdda_engine.c")
        .opt_level(3)
        .flag_if_supported("-pthread")
        .compile("cdda_engine");

    println!("cargo:rustc-link-lib=asound");
}
