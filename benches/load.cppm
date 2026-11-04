export module vecgltf.bench.load;
import :options;
import :cpu;
import :startup;
import rstd;

int main(int argc, char** argv) {
    auto parsed = vecgltf_bench::parse(argc, argv);
    if (parsed.is_err()) {
        rstd::io::eprintln("{}", parsed.unwrap_err());
        return 2;
    }
    auto selected = parsed.unwrap();
    if (selected.is_none()) return 0;
    auto options = rstd::move(selected).unwrap();
    auto result  = options.mode == vecgltf_bench::Mode::Cpu
                       ? vecgltf_bench::RunCpu(rstd::move(options.cpu))
                       : vecgltf_bench::RunStartup(rstd::move(options.startup));
    if (result.is_err()) {
        rstd::io::eprintln("{}", result.unwrap_err());
        return 1;
    }
    return 0;
}
