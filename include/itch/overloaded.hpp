#pragma once

namespace itch {

// Lets std::visit take one lambda per message type.
template <typename... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

}  // namespace itch
