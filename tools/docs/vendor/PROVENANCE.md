# CyDoc build dependency

CyDoc 0.1.0, MIT license, built from the private repository
`https://github.com/MrMybal/CyDoc` at commit
`774f2a7eabf7dd181a98ffc5add115b580b67f42`.

The archive was produced with `npm ci`, `npm run build`, then `npm pack
--ignore-scripts`. It is a build dependency, not a browser dependency. Its license
is preserved in the package and copied to the generated manual.

Application installs do not require GitHub authentication, Node.js, npm, Three.js,
or a web server to read the distributed manual. The application's generation
package has no Three.js dependency; only the image and gallery bundles are exported.

To update the dependency, read the four `ai/` integration guides in the desired
CyDoc commit, rebuild the package, update the archive and lockfile, and rerun the
manual validation and local-file browser checks. Do not edit CyDoc's generated
bundles or apply CyDoc's own repository instructions to this application.
