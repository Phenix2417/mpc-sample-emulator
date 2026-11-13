# Expo development server for Expo Go, hosted on Railway (see README: "Open in Expo Go").
FROM node:22-slim
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY . .
ENV CI=1 EXPO_NO_TELEMETRY=1 NODE_ENV=development
# Railway gives the public address in RAILWAY_PUBLIC_DOMAIN and the port in PORT; Expo Go is pointed at the address.
CMD ["sh", "-c", "export EXPO_PACKAGER_PROXY_URL=https://${RAILWAY_PUBLIC_DOMAIN}; echo \"Expo Go address: exps://${RAILWAY_PUBLIC_DOMAIN}\"; exec npx expo start --port ${PORT:-8081}"]
