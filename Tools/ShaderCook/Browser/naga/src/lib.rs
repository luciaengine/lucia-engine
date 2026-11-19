use std::cell::RefCell;

thread_local! {
    static INPUT: RefCell<Vec<u32>> = const { RefCell::new(Vec::new()) };
    static OUTPUT: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

#[unsafe(no_mangle)]
pub extern "C" fn input_buffer(word_count: usize) -> *mut u32 {
    INPUT.with_borrow_mut(|input| {
        input.resize(word_count, 0);
        input.as_mut_ptr()
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn translate_spirv() -> i32 {
    let result = INPUT.with_borrow(|input| {
        let options = naga::front::spv::Options {
            adjust_coordinate_space: false,
            ..Default::default()
        };
        let module = naga::front::spv::Frontend::new(input.iter().copied(), &options)
            .parse()
            .map_err(|error| error.to_string())?;
        let info = naga::valid::Validator::new(
            naga::valid::ValidationFlags::all(),
            naga::valid::Capabilities::all(),
        )
        .validate(&module)
        .map_err(|error| error.to_string())?;
        naga::back::wgsl::write_string(&module, &info, naga::back::wgsl::WriterFlags::EXPLICIT_TYPES)
            .map_err(|error| error.to_string())
    });
    let (status, text) = match result {
        Ok(wgsl) => (0, wgsl),
        Err(error) => (1, error),
    };
    OUTPUT.with_borrow_mut(|output| *output = text.into_bytes());
    status
}

#[unsafe(no_mangle)]
pub extern "C" fn output_buffer() -> *const u8 {
    OUTPUT.with_borrow(|output| output.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn output_size() -> usize {
    OUTPUT.with_borrow(|output| output.len())
}
