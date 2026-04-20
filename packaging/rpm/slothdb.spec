Name:           slothdb
Version:        0.1.3
Release:        1%{?dist}
Summary:        An embedded analytical database engine

License:        MIT
URL:            https://github.com/SouravRoy-ETL/slothdb
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.14
BuildRequires:  gcc-c++ >= 12

%description
SlothDB is a production-grade in-process OLAP database written in C++20.
Zero dependencies. GPU accelerated. Query CSV, Parquet, JSON, Excel directly.
130+ SQL features including window functions, QUALIFY, CTEs, and more.

%prep
%setup -q

%build
cmake -B build -DSLOTHDB_BUILD_SHELL=ON -DSLOTHDB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j%{_smp_mflags}

%install
mkdir -p %{buildroot}%{_bindir}
install -m 755 build/src/slothdb %{buildroot}%{_bindir}/slothdb

%files
%license LICENSE
%doc README.md
%{_bindir}/slothdb

%changelog
* Mon Apr 20 2026 Sourav Roy <souravroy7864@gmail.com> - 0.1.3-1
- Arrow IPC + SQLite now stream typed DataChunks (PhysicalArrowScan / PhysicalSQLiteScan)
- All 7 file formats on the fast path — no more bulk-load-to-DataTable roundtrip
* Sun Apr 19 2026 Sourav Roy <souravroy7864@gmail.com> - 0.1.2-1
- Performance: beats DuckDB 1.1x-6.6x on every format (CSV, Parquet, JSON, Avro, Excel)
* Wed Apr 16 2026 Sourav Roy <souravroy7864@gmail.com> - 0.1.0-1
- Initial release
