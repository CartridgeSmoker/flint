<h1 align="center">Flint</h1>

<p align="center">
  <a href="https://github.com/podomy/flint/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/podomy/flint/ci.yml?label=linux" alt="Linux"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/podomy/flint" alt="License"></a>
</p>

Flint is a high-performance framework for technical desktop applications. It is a cross-platform framework for GPU-intensive applications, combining production-ready UI components with high-performance rendering on Windows, Linux, and macOS.

Flint is opinionated and utility-focused. Application structure such as sidebars, panels, tables, and viewports ships premade, so a new application bootstraps quickly and most Flint applications look largely the same. Rendering goes through a single Vulkan path, so conventional UI and intensive views such as 2D/3D viewports, real-time graphs, and large datasets are drawn by the same backend.

### Who it is for

- Creative and engineering software: CAD, 2D/3D editors, image processing, compositing, simulation.
- Industrial visualization: real-time monitoring, process control interfaces, equipment diagnostics.
- Advanced technical tools: profilers, scientific visualization, large-data explorers, node-based editors.

### Status

Early development. The public C API lives in `include/flint/` with a stable opaque-handle ABI. See [Developing](./docs/developing.md) for dependencies and build steps.

### Contributing

Discuss your change with the engineering team at [contact@podomy.com](mailto:contact@podomy.com) before opening a PR in order not to waste anybody's effort or time.

Announcements and engineering updates are shared via our newsletter at [podomy.com](https://podomy.com).

- [Commit message format](./COMMITS)
- [Contributor license agreement](./CLA)
- [Contributing](./CONTRIBUTING)

### License

Flint is distributed under the GNU Affero General Public License v3.0 or
later. See [LICENSE](./LICENSE).
