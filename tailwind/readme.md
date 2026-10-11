# Build Tailwind CSS

from `dashboard_app/tailwind`
```bash
npm install tailwindcss @tailwindcss/cli
```

Build the css from from `./`
```bash
npx @tailwindcss/cli -i ./tailwind/input.css -o ./static/css/build.css --watch --minify
# or
npx @tailwindcss/cli -i ./tailwind/input.css -o ./static/css/build.css 
```
